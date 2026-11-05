-- "Send this log to Outrigger" (a failed install, site/installer/report_form.js openLogForm): its log rides in
-- the report, with every MAC removed by the page and again by functions/api/report.js. Apply with
-- tools/reports.py migrate, on both databases.
ALTER TABLE reports ADD COLUMN log TEXT;
