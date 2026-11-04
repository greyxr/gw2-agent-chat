import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';

const executable = process.argv[2];
assert.ok(executable);
const auth = `Basic ${Buffer.from('opencode:gw2-agent-chat-test-password').toString('base64')}`;
const quotedPrompt = 'a"b\u732B';
const prompts = [];
let sessions = 0;
let aborted = false;
let cancelDone;
let child;

const server = createServer(async (request, response) => {
  response.on('error', () => {});
  if (request.headers.authorization !== auth) return response.writeHead(401).end();
  if (request.method === 'POST' && request.url === '/session') {
    sessions += 1;
    return response.writeHead(200, { 'content-type': 'application/json' }).end('{"id":"ses_fixture"}');
  }
  if (request.method === 'POST' && request.url === '/session/ses_fixture/message') {
    let body = '';
    for await (const part of request) body += part;
    const prompt = JSON.parse(body);
    assert.equal(prompt.agent, 'gw2');
    assert.equal(prompt.parts[0].type, 'text');
    const text = prompt.parts[0].text;
    prompts.push(text);
    if (text === 'cancel') {
      child.stdin.write('\n');
      await new Promise(resolve => { cancelDone = resolve; });
      return;
    }
    if (text === 'error') return response.writeHead(400).end('{"error":"failed"}');
    if (text === 'large') {
      const oversized = { info: { role: 'assistant' }, parts: [{ type: 'text', text: 'x'.repeat((2 << 20) + 1) }] };
      return response.writeHead(200, { 'content-type': 'application/json' }).end(JSON.stringify(oversized));
    }
    assert.ok(text === 'first' || text === quotedPrompt);
    const reply = text === 'first' ? 'reply one' : 'reply two';
    return response.writeHead(200, { 'content-type': 'application/json' }).end(JSON.stringify({
      info: { id: `msg_${prompts.length}`, role: 'assistant' },
      parts: [{ type: 'text', text: reply }],
    }));
  }
  if (request.method === 'POST' && request.url === '/session/ses_fixture/abort') {
    aborted = true;
    cancelDone?.();
    return response.writeHead(204).end();
  }
  response.writeHead(404).end();
});

server.listen(0, '127.0.0.1', () => {
  child = spawn(executable, [String(server.address().port)], { stdio: ['pipe', 'inherit', 'inherit'] });
  child.once('error', error => {
    console.error(error.message);
    server.close(() => process.exit(1));
  });
  child.once('exit', (code, signal) => {
    try {
      assert.equal(signal, null);
      assert.equal(code, 0);
      assert.equal(sessions, 1);
      assert.deepEqual(prompts, ['first', quotedPrompt, 'error', 'large', 'cancel']);
      assert.equal(aborted, true);
    } catch (error) {
      console.error(error.message);
      process.exitCode = 1;
    }
    server.close();
  });
});
