'use strict';
// Independent JSON parser + schema-subset validator, using only Node's standard library.
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '..');
const build = path.resolve(root, process.argv[2] || 'build-release');
const artifact = path.join(root, 'artifacts');
fs.mkdirSync(artifact, { recursive: true });
const schema = JSON.parse(fs.readFileSync(path.join(root, 'docs/schema.json'), 'utf8'));
function validate(value, rule, where = '$') {
    if (rule.const !== undefined) assert.deepEqual(value, rule.const, where);
    if (rule.enum) assert(rule.enum.includes(value), `${where}: enum`);
    const type = value === null ? 'null' : Array.isArray(value) ? 'array' : typeof value === 'number' && Number.isInteger(value) ? 'integer' : typeof value;
    if (rule.type) assert([].concat(rule.type).includes(type), `${where}: type ${type}`);
    if (rule.minimum !== undefined) assert(value >= rule.minimum, `${where}: minimum`);
    if (rule.maximum !== undefined) assert(value <= rule.maximum, `${where}: maximum`);
    if (rule.pattern) assert(new RegExp(rule.pattern).test(value), `${where}: pattern`);
    if (rule.required) for (const key of rule.required) assert(Object.hasOwn(value, key), `${where}: required ${key}`);
    if (rule.properties) for (const [key, sub] of Object.entries(rule.properties)) if (Object.hasOwn(value, key)) validate(value[key], sub, `${where}.${key}`);
    if (rule.maxItems !== undefined) assert(value.length <= rule.maxItems, `${where}: maxItems`);
    if (rule.items) value.forEach((item, i) => validate(item, rule.items, `${where}[${i}]`));
    if (rule.not) assert.throws(() => validate(value, rule.not, where), undefined, `${where}: not`);
    if (rule.oneOf) {
        const passing = rule.oneOf.filter(sub => { try { validate(value, sub, where); return true; } catch { return false; } });
        assert.equal(passing.length, 1, `${where}: oneOf`);
    }
}
function run(exe, args, exit, name, isJson = true) {
    const result = spawnSync(path.join(build, exe), args, { encoding: 'utf8', timeout: 15000 });
    if (result.error) throw result.error;
    assert.equal(result.status, exit, `${name}: exit; ${result.stdout} ${result.stderr}`);
    assert.equal(result.stderr, '', `${name}: stderr must be clean`);
    fs.writeFileSync(path.join(artifact, name + (isJson ? '.json' : '.txt')), result.stdout);
    if (isJson) {
        const parsed = JSON.parse(result.stdout);
        validate(parsed, schema);
        return parsed;
    }
    return result.stdout;
}
const cli = 'scheduled-task-failure-explainer.exe';
const fixture = run('stfe_tests.exe', [], 0, 'synthetic-fixture');
assert.equal(fixture.task.path, '\\Folder\\工作😀');
assert.equal(fixture.actions.length, 2);
assert.equal(fixture.triggers.length, 2);
assert.equal(fixture.history.events.length, 1);
run(cli, ['--json', '\\STFE-Nonexistent-Folder-577B8CB4\\missing'], 3, 'missing-folder');
run(cli, ['--json', '\\Microsoft\\Windows\\Defrag\\STFE-Nonexistent-577B8CB4'], 3, 'missing-task');
run(cli, ['--json', '\\工作😀不存在'], 3, 'unicode-missing-task');
run(cli, ['--json', '--unknown'], 2, 'invalid-cli');
run(cli, ['--json'], 2, 'missing-cli-path');
run(cli, ['--json', '\\task\\'], 2, 'invalid-task-path');
assert.match(run(cli, ['--version'], 0, 'version', false), /1\.0\.0/);
assert.match(run(cli, ['--help'], 0, 'help', false), /Read-only/);
const real = run(cli, ['--json', '\\Microsoft\\Windows\\Defrag\\ScheduledDefrag'], 0, 'real-defrag');
assert.equal(real.task.path, '\\Microsoft\\Windows\\Defrag\\ScheduledDefrag');
assert(real.history.status === 'disabled' || real.history.status === 'available' || real.history.status === 'access_denied' || real.history.status === 'unavailable');
run(cli, ['\\Microsoft\\Windows\\Defrag\\ScheduledDefrag'], 0, 'real-defrag-human', false);
console.log(`Independent JSON parsing and schema checks passed: synthetic, real task (${real.history.status} history), CLI and acquisition errors, Unicode, text/version/help.`);
