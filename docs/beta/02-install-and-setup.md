# Install and first setup

**Installing is on the homepage: [outrigger.music](https://outrigger.music/#install), under "Set up once".** Plug your
ESP32 into the computer, click **Check my board** and pick the port Chrome offers. It reads the board and tells you
what installing would change, without changing anything. Then click **Install**. It never erases: your settings and
FM-1 backups stay. On a new ESP32 it also sets up firmware switching with three FM-1 firmwares to switch between:
M-VAVE stock V15, Felucca 1.8.0 and Skiff 2.1.1. Your browser downloads them from the Firmware Hub, and one
small file from GitHub, and checks each. If a download fails, it writes nothing and asks you to try again.

If it says your ESP32 **has storage we can't read**, nothing was installed. That can happen on an ESP32 that ran an
earlier beta. Email [feedback@outrigger.music](mailto:feedback@outrigger.music) before going further: your backups may
be in there.

Then put it on the hub:

1. The charger goes into the hub's charging port, first.
2. Outrigger goes on the hub's host cable or port, the one a laptop would use.
3. The FM-1 and your MIDI controller go into the hub's data ports.

Outrigger's **Play** screen shows a line each for the hub, your controller, the router and the FM-1. All green means
notes are getting through.

**Wi-Fi:** the Install button gives you Outrigger without Wi-Fi. Wi-Fi and Outrigger's own web page are coming;
see "What's coming" on the homepage.

## Updating

When a new build is out:

1. On Outrigger, open **Settings**, then **Computer mode**, and hold the button. Outrigger restarts with MIDI off, so
   your computer can reach its USB port.
2. Plug it into your computer, open [outrigger.music](https://outrigger.music/#install), click **Check my board**,
   then **Install**.
3. When it's done, Outrigger restarts by itself and goes back to MIDI. Your settings and backups stay.

**Settings** shows which build you're running, for example `ccbc36d hub+L`.

## Going back to an earlier build

Not from the website yet: it only offers the latest build. If a new build gives you trouble, email
[feedback@outrigger.music](mailto:feedback@outrigger.music) with the build shown in Settings, and we'll get you the
previous one.
