'use strict';
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');
const exe = path.join(root, process.argv[2] || 'build-release', 'scheduled-task-failure-explainer.exe');
const tasks = [
    '\\Microsoft\\Windows\\Defrag\\ScheduledDefrag',
    '\\Microsoft\\Windows\\Registry\\RegIdleBackup',
    '\\Microsoft\\Windows\\WindowsUpdate\\Scheduled Start',
    '\\Microsoft\\Windows\\Time Synchronization\\SynchronizeTime',
    '\\Microsoft\\Windows\\Maintenance\\WinSAT',
    '\\Microsoft\\Windows\\UpdateOrchestrator\\Schedule Scan'
];
fs.mkdirSync(path.join(root, 'artifacts'), { recursive: true });
const summary = [];
for (const [i, task] of tasks.entries()) {
    const r = spawnSync(exe, ['--json', task], { encoding: 'utf8', timeout: 15000 });
    if (r.error) throw r.error;
    if (r.stderr || ![0, 3].includes(r.status)) throw new Error(`Unexpected integration failure: ${task}: ${r.status}: ${r.stderr}`);
    const report = JSON.parse(r.stdout);
    fs.writeFileSync(path.join(root, 'artifacts', `integration-${i}.json`), r.stdout);
    summary.push({ task, exit: r.status, history: report.history?.status, actions: report.actions?.map(a => a.type), triggers: report.triggers?.map(t => t.type), result: report.last_result?.hex, error: report.error });
}
fs.writeFileSync(path.join(root, 'artifacts', 'integration-summary.json'), JSON.stringify(summary, null, 2) + '\n');
console.log(JSON.stringify(summary, null, 2));
