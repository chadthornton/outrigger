"""The checker as it stood on night 0 (tools/robot.py at e8ef578, all in
memory), kept as the reference that tests/test_robot.py holds the windowed
Checker and the lazy plan() to. Not used by tools/robot.py."""
import bisect
import collections
import statistics


def bind(robot):
    """The night-0 check(), expand() and scenario_end_us(), using robot.py's helpers (msg_len, passes, mapped ...)."""
    msg_len, passes, mapped, kind_of, ms_us, pct = (robot.msg_len, robot.passes, robot.mapped, robot.kind_of,
                                                     robot.ms_us, robot.pct)
    UNPLUG_GRACE_US = robot.UNPLUG_GRACE_US
    RobotError = robot.RobotError

    def expand(scn):
        """{robot: {"at": [(t_rel_us, line)], "rep": [line]}} for a scenario's streams."""
        out = {r: {"at": [], "rep": []} for r in scn["robots"]}
        for st in scn.get("streams", []):
            rb = st["robot"]
            if rb not in out:
                raise RobotError(f"stream for robot {rb}, which the scenario doesn't list")
            t0 = ms_us(st.get("start_ms", 0))
            cable = st.get("cable", 0)
            at = out[rb]["at"]

            def midi(t, *b):
                at.append((t, f"at {t} {cable} {bytes(b).hex().upper()}"))
            if "notes" in st:
                n = st["notes"]
                ch = n.get("channel", 1) - 1
                keys = n.get("notes", [60])
                for i in range(n["count"]):
                    t = t0 + ms_us(i * n["every_ms"])
                    k = keys[i % len(keys)]
                    midi(t, 0x90 | ch, k, n.get("velocity", 100))
                    midi(t + ms_us(n.get("gate_ms", n["every_ms"] / 2)), 0x80 | ch, k, 0)
            if "cc" in st:
                c = st["cc"]
                ch = c.get("channel", 1) - 1
                lo, hi, cnt = c.get("from", 0), c.get("to", 127), c["count"]
                for i in range(cnt):
                    v = lo + round((hi - lo) * i / max(cnt - 1, 1))
                    midi(t0 + ms_us(i * c["every_ms"]), 0xB0 | ch, c["cc"], v)
            if "clock" in st:
                c = st["clock"]
                interval_ns = round(60e9 / (c["bpm"] * 24))
                if c.get("start", True):
                    midi(t0, 0xFA)
                out[rb]["rep"].append(f"rep {t0} {interval_ns} {c['ticks']} {cable} F8")
                if c.get("stop", True):
                    midi(t0 + (c["ticks"] * interval_ns) // 1000 + 1, 0xFC)
            if "unplug" in st:
                u = st["unplug"]
                for i in range(u["count"]):
                    t = t0 + ms_us(i * u["every_ms"])
                    at.append((t, f"at {t} unplug {int(u['down_ms'])}"))
            for t_ms, hx in st.get("messages", []):
                t = t0 + ms_us(t_ms)
                at.append((t, f"at {t} {cable} {hx.upper()}"))
        for r in out.values():
            r["at"].sort(key=lambda e: e[0])
        return out


    def scenario_end_us(plan):
        end = 0
        for r in plan.values():
            if r["at"]:
                end = max(end, r["at"][-1][0])
            for line in r["rep"]:
                _, start, ns, count, _, _ = line.split()
                end = max(end, int(start) + int(ns) * int(count) // 1000)
        return end


    def clock_model(d):
        """robot µs -> Mac µs, and its uncertainty, from the start and end time requests."""
        (m0, r0, rtt0), (m1, r1, rtt1) = d["sync"][0], d["sync"][-1]
        scale = (m1 - m0) / (r1 - r0) if r1 != r0 else 1.0
        return (lambda t: m0 + (t - r0) * scale), max(rtt0, rtt1) / 2
    
    def outages(recs, to_mac, reenum_us):
        """(start, end) Mac µs windows from each unplug to the host's next mount, plus a grace.
        A mount that never comes closes the window reenum_us after the plug, so a
        robot whose host never sees it again can't excuse every later loss."""
        out, start, plug = [], None, None
        for kind, t, _ in sorted(recs, key=lambda r: r[1]):
            if kind == "u" and start is None:
                start, plug = to_mac(t), None
            elif kind == "p" and start is not None:
                plug = to_mac(t)
            elif kind == "M" and start is not None and (plug is None or to_mac(t) <= plug + reenum_us):
                out.append((start - UNPLUG_GRACE_US, to_mac(t) + UNPLUG_GRACE_US))
                start = plug = None
            elif start is not None and plug is not None and to_mac(t) > plug + reenum_us:
                out.append((start - UNPLUG_GRACE_US, plug + reenum_us + UNPLUG_GRACE_US))
                start = plug = None
        if start is not None:
            end = plug + reenum_us + UNPLUG_GRACE_US if plug is not None else float("inf")
            out.append((start - UNPLUG_GRACE_US, end))
        return out
    
    def check(scn, data):
        """The checks for one run: {"checks": {name: {ok, value, limit}}, "numbers": {...}, "ok": bool}."""
        lim = scn.get("checks", {})
        models = {n: clock_model(d) for n, d in data.items()}
        checks, numbers = {}, {}
    
        def put(name, value, limit, ok):
            checks[name] = {"ok": ok, "value": value, "limit": limit}
    
        # accounted: the logs hold every packet the robots counted.
        gaps = []
        for n, d in data.items():
            st = d["status"]
            s = sum(1 for k, _, _ in d["records"] if k == "s")
            r = sum(1 for k, _, _ in d["records"] if k == "r")
            if s != st["sent"] or r != st["recv"] or st["log_lost_tx"] or st["log_lost_rx"]:
                gaps.append(f"{n}: sent {st['sent']} logged {s}, recv {st['recv']} logged {r}, "
                            f"log lost {st['log_lost_tx']}+{st['log_lost_rx']}")
        put("accounted", "; ".join(gaps) or "all", "all", not gaps)
    
        reenum_us = ms_us(lim.get("reenum_ms_max", 2000))
        down = {n: outages(d["records"], models[n][0], reenum_us) for n, d in data.items()}
    
        def in_outage(robot_names, t_mac):
            return any(a <= t_mac <= b for n in robot_names for a, b in down[n])
    
        # What each route expects, keyed by where it lands and what it is.
        expect = collections.defaultdict(list)
        routes = scn.get("routes", [])
        for ri, rt in enumerate(routes):
            src, dst = rt["from"], rt["to"]
            to_mac = models[src["robot"]][0]
            seq = 0
            for kind, t, pkt in data[src["robot"]]["records"]:
                if kind != "s" or pkt[0] >> 4 != src.get("cable", 0):
                    continue
                msg = pkt[1:1 + msg_len(pkt[1])]
                if not passes(rt.get("filter"), msg):
                    continue
                want = mapped(rt.get("map"), msg)
                expect[(dst["robot"], dst.get("cable", 0), want)].append(
                    {"route": ri, "seq": seq, "t": t, "t_mac": to_mac(t), "src": src["robot"]})
                seq += 1
        for key in expect:
            expect[key] = sorted(expect[key], key=lambda e: e["t_mac"])
    
        # Arrivals, each matched to an expectation of the same content. First an
        # estimate of the latency: each arrival against the latest expectation sent
        # before it. Then each arrival takes the unmatched expectation nearest
        # (arrival - that latency); one further off than the match window is an
        # extra (a duplicate), and expectations passed over are lost. So one lost
        # or doubled message among identical ones (clock ticks) is the one counted,
        # as long as identical messages are further apart than latency + jitter.
        dest_cables = {(rt["to"]["robot"], rt["to"].get("cable", 0)) for rt in routes}
        # listen: a cable that takes what the P4 makes itself (its clock), checked
        # against the scenario's numbers rather than a robot's sends.
        listens = {(ls["robot"], ls.get("cable", 0)): ls for ls in scn.get("listen", [])}
        heard = collections.defaultdict(list)
        arrivals, extras = [], 0
        for n, d in data.items():
            to_mac = models[n][0]
            for kind, t, pkt in d["records"]:
                if kind != "r":
                    continue
                cable = pkt[0] >> 4
                msg = pkt[1:1 + msg_len(pkt[1])]
                ls = listens.get((n, cable))
                if ls and passes(ls.get("filter", {"types": ["clock", "transport"]}), msg):
                    heard[(n, cable)].append((t, msg))
                elif (n, cable) not in dest_cables:
                    extras += 1
                else:
                    arrivals.append((to_mac(t), t, n, cable, msg))
        arrivals.sort(key=lambda a: a[0])
    
        times = {k: [e["t_mac"] for e in q] for k, q in expect.items()}
        est = []
        for a in arrivals:
            ts = times.get((a[2], a[3], a[4]))
            if ts:
                i = bisect.bisect_right(ts, a[0]) - 1
                if i >= 0:
                    est.append(a[0] - ts[i])
        typical = statistics.median(est) if est else 0
        mad = statistics.median(abs(x - typical) for x in est) if est else 0
        window = ms_us(lim["match_window_ms"]) if "match_window_ms" in lim else max(5000, 8 * mad)
        numbers["match_window_ms"] = round(window / 1000, 3)
    
        pos = collections.defaultdict(int)
        pairs, missing = [], []
        for a in arrivals:
            k = (a[2], a[3], a[4])
            q = expect.get(k)
            i = pos[k]
            if not q or i >= len(q):
                extras += 1
                continue
            target = a[0] - typical
            j = i
            while j + 1 < len(q) and abs(q[j + 1]["t_mac"] - target) < abs(q[j]["t_mac"] - target):
                j += 1
            if abs(q[j]["t_mac"] - target) > window:
                extras += 1
                continue
            missing += [(a[2], e) for e in q[i:j]]
            pairs.append((a, q[j]))
            pos[k] = j + 1
        missing += [(k[0], e) for k, q in expect.items() for e in q[pos[k]:]]
    
        lat, order_bad, cross = [], 0, False
        last_seq = {}
        clock_rx = collections.defaultdict(list)
        held = collections.Counter()
        for (t_mac, t, n, cable, msg), e in pairs:
            same = e["src"] == n
            cross |= not same
            lat.append((t - e["t"]) if same else (t_mac - e["t_mac"]))
            key = (e["route"], n, cable)
            if e["seq"] < last_seq.get(key, -1):
                order_bad += 1
            last_seq[key] = max(e["seq"], last_seq.get(key, -1))
            if msg[0] == 0xF8:
                clock_rx[key].append(t)
        # Stuck notes: what each receiver holds down, in arrival order.
        for t_mac, t, n, cable, msg in arrivals:
            if kind_of(msg) == "note" and msg[0] < 0xA0:
                k = (n, cable, msg[0] & 0x0F, msg[1])
                if msg[0] >> 4 == 0x9 and msg[2] > 0:
                    held[k] += 1
                elif held[k] > 0:
                    held[k] -= 1
    
        lost = lost_unplugged = 0
        for dr, e in missing:
            if in_outage({dr, e["src"]}, e["t_mac"]):
                lost_unplugged += 1
            else:
                lost += 1
        put("loss", lost, lim.get("loss_max", 0), lost <= lim.get("loss_max", 0))
        numbers["lost_unplugged"] = lost_unplugged
        put("extra", extras, lim.get("extra_max", 0), extras <= lim.get("extra_max", 0))
        if lim.get("order", True):
            put("order", order_bad, 0, order_bad == 0)
        numbers["matched"] = len(lat)
        if lat:
            ms = [x / 1000 for x in lat]
            numbers.update(latency_ms_p50=round(pct(ms, 50), 3), latency_ms_p99=round(pct(ms, 99), 3),
                           latency_ms_max=round(max(ms), 3), latency_ms_min=round(min(ms), 3),
                           jitter_ms=round(pct(ms, 99) - pct(ms, 50), 3))
            numbers["latency_basis_us"] = (round(max(models[n][1] for n in data)) if cross else 0)
            if "latency_ms_max" in lim:
                put("latency", numbers["latency_ms_max"], lim["latency_ms_max"],
                    numbers["latency_ms_max"] <= lim["latency_ms_max"])
            if "jitter_ms_max" in lim:
                put("jitter", numbers["jitter_ms"], lim["jitter_ms_max"], numbers["jitter_ms"] <= lim["jitter_ms_max"])
        worst = None
        for (ri, n, cable), ts in clock_rx.items():
            # The nominal interval is the sender's own: the rep line's grid.
            sent = [e for e in (r for r in data[routes[ri]["from"]["robot"]]["records"])
                    if e[0] == "s" and e[2][1] == 0xF8 and e[2][0] >> 4 == routes[ri]["from"].get("cable", 0)]
            if len(ts) < 3 or len(sent) < 3:
                continue
            nominal = (sent[-1][1] - sent[0][1]) / (len(sent) - 1)
            devs = [abs((b - a) - nominal) / 1000 for a, b in zip(ts, ts[1:])]
            worst = max(worst or 0, max(devs))
            numbers["clock_interval_ms_stdev"] = round(statistics.pstdev([(b - a) / 1000 for a, b in zip(ts, ts[1:])]), 4)
        for key, got in heard.items():
            ls = listens[key]
            ts = [t for t, m in got if m[0] == 0xF8]
            numbers[f"{key[0]}{key[1]}_heard"] = len(got)
            if "clock" in ls and len(ts) >= 3:
                nominal = 60e6 / (ls["clock"]["bpm"] * 24)
                devs = [abs((b - a) - nominal) / 1000 for a, b in zip(ts, ts[1:])]
                worst = max(worst or 0, max(devs))
                numbers[f"{key[0]}{key[1]}_clock_bpm"] = round(60e6 / ((ts[-1] - ts[0]) / (len(ts) - 1)) / 24, 3)
                if "ticks_min" in ls["clock"]:
                    put(f"clock_ticks_{key[0]}{key[1]}", len(ts), ls["clock"]["ticks_min"], len(ts) >= ls["clock"]["ticks_min"])
        if worst is not None:
            numbers["clock_jitter_ms"] = round(worst, 4)
            if "clock_jitter_ms_max" in lim:
                put("clock_jitter", round(worst, 4), lim["clock_jitter_ms_max"], worst <= lim["clock_jitter_ms_max"])
        stuck = sum(v for v in held.values() if v > 0)
        put("stuck_notes", stuck, lim.get("stuck_notes_max", 0), stuck <= lim.get("stuck_notes_max", 0))
    
        reconnect, plugs = [], 0
        for n, d in data.items():
            plug_t = None
            for kind, t, _ in sorted(d["records"], key=lambda r: r[1]):
                if kind == "p":
                    plugs += 1
                    plug_t = t
                elif kind == "M" and plug_t is not None:
                    if t - plug_t <= reenum_us:
                        reconnect.append((t - plug_t) / 1000)
                    plug_t = None
            numbers[f"{n}_not_sent"] = d["status"]["not_sent"]
            numbers[f"{n}_unplugs"] = d["status"]["unplugs"]
        if plugs:
            # Every replug must end in the host mounting the robot again, in time.
            put("remounted", f"{len(reconnect)}/{plugs}", f"all, within {reenum_us // 1000} ms", len(reconnect) == plugs)
        if reconnect:
            numbers.update(reconnect_ms_max=round(max(reconnect), 1), reconnect_ms_p50=round(pct(reconnect, 50), 1),
                           reconnects=len(reconnect))
        return {"ok": all(c["ok"] for c in checks.values()), "checks": checks, "numbers": numbers}

    return {"check": check, "expand": expand, "scenario_end_us": scenario_end_us}
