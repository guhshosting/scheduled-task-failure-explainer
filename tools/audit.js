'use strict';
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '..');
const files = fs.readdirSync(path.join(root, 'src')).filter(f => /\.(cpp|hpp)$/.test(f));
const forbidden = [
    /(?:->|\.)(?:Run|RunEx|Stop|StopTask|RegisterTask|RegisterTaskDefinition|DeleteTask|DeleteFolder|CreateFolder|SetSecurityDescriptor|put_\w+)\s*\(/g,
    /\b(?:EvtSetChannelConfigProperty|EvtSaveChannelConfig|EvtClearLog|EvtExportLog|CreateProcess\w*|ShellExecute\w*|WinHttp\w*|InternetOpen\w*|WSAStartup|GetAddrInfo\w*|getaddrinfo|DnsQuery\w*|URLDownload\w*|system|_wsystem|popen|_popen)\s*\(/g,
    /\b(?:GENERIC_WRITE|FILE_WRITE_DATA|FILE_APPEND_DATA|CREATE_ALWAYS|OPEN_ALWAYS|TRUNCATE_EXISTING)\b/g
];
let output = 'Production-source read-only/no-intentional-network audit\n';
for (const file of files) {
    const text = fs.readFileSync(path.join(root, 'src', file), 'utf8');
    for (const pattern of forbidden) assert.equal([...text.matchAll(pattern)].length, 0, `${file}: forbidden API / access mode ${pattern}`);
    output += `${file}: deny-list scan passed\n`;
    if (file === 'acquire.cpp') {
        const calls = [...text.matchAll(/\b(?:[A-Z][A-Za-z0-9_]*(?:W)?|get_\w+)\s*\(/g)].map(m => m[0].replace(/\s*\($/, ''));
        output += `Native API/getter inventory: ${[...new Set(calls)].sort().join(', ')}\n`;
        assert(text.includes('service->Connect(empty, empty, empty, empty)'));
        assert(text.includes('EvtQuery(nullptr, channel'));
        assert(text.includes('EvtOpenChannelConfig(nullptr, channel'));
        assert(text.includes('EvtOpenPublisherMetadata(nullptr'));
        assert(text.includes('FILE_FLAG_OPEN_REPARSE_POINT'));
        assert(text.includes('QueryDosDeviceW'));
        assert(text.includes('FILE_READ_ATTRIBUTES, FILE_SHARE_READ'));
        assert(text.includes('OPEN_EXISTING'));
        output += 'Local-only COM/Event Log sessions and guarded read-only path checks verified.\n';
    }
}
const manifest = fs.readFileSync(path.join(root, 'src/app.manifest'), 'utf8');
assert(manifest.includes('level="asInvoker"'));
output += 'Explicit asInvoker manifest verified.\n';
output += 'Limit: source/API inventory review establishes intent; this is not a packet capture or a formal proof of OS internals.\n';
fs.mkdirSync(path.join(root, 'artifacts'), { recursive: true });
fs.writeFileSync(path.join(root, 'artifacts/source-audit.txt'), output);
console.log(output);
