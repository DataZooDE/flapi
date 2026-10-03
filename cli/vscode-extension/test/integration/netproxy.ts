import net from 'node:net';
import http from 'node:http';
import https from 'node:https';
import os from 'node:os';
import path from 'node:path';
import fs from 'node:fs';
import { execFileSync } from 'node:child_process';

/**
 * Network faults in front of the REAL flapi server. Nothing here answers for flapi:
 * every byte that reaches a client is flapi's own, or the connection just dies.
 */

/** TCP proxy that, once, forwards a request of `dropMethod` and then cuts the client
 *  off without relaying the answer ("server committed, answer lost"). It records the
 *  request head of every request that reaches the server. */
export async function lossyProxy(upstreamUrl: string, dropMethod: string) {
  const upstream = new URL(upstreamUrl);
  const heads: string[] = [];
  let dropped = false;

  const server = net.createServer((client) => {
    const up = net.connect(Number(upstream.port), upstream.hostname);
    let first = true;
    client.on('data', (chunk) => {
      if (first) {
        first = false;
        heads.push(chunk.toString('latin1'));
        const method = chunk.toString('latin1', 0, 8).split(' ')[0];
        if (method === dropMethod && !dropped) {
          dropped = true;
          up.write(chunk);
          up.once('data', () => client.destroy());
          return;
        }
      }
      up.write(chunk);
    });
    up.on('data', (d) => client.writable && client.write(d));
    up.on('close', () => client.end());
    client.on('close', () => up.destroy());
    client.on('error', () => up.destroy());
    up.on('error', () => client.destroy());
  });
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  const port = (server.address() as net.AddressInfo).port;
  return {
    url: `http://127.0.0.1:${port}`,
    heads,
    count: (method: string) => heads.filter((h) => h.startsWith(method + ' ')).length,
    close: () =>
      new Promise<void>((resolve) => {
        server.close(() => resolve());
        server.closeAllConnections?.();
      }),
  };
}

/** Accepts connections and never answers. */
export async function blackhole() {
  const sockets = new Set<net.Socket>();
  const server = net.createServer((s) => {
    sockets.add(s);
    s.on('error', () => {});
  });
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  const port = (server.address() as net.AddressInfo).port;
  return {
    url: `http://127.0.0.1:${port}`,
    close: () =>
      new Promise<void>((resolve) => {
        sockets.forEach((s) => s.destroy());
        server.close(() => resolve());
      }),
  };
}

/** HTTPS front (self-signed certificate) that relays to the real flapi over HTTP. */
export async function tlsFront(upstreamUrl: string) {
  const upstream = new URL(upstreamUrl);
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'flapi-tls-'));
  const key = path.join(dir, 'k.pem');
  const cert = path.join(dir, 'c.pem');
  execFileSync(
    'openssl',
    ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', key, '-out', cert, '-days', '1',
      '-subj', '/CN=127.0.0.1', '-addext', 'subjectAltName=IP:127.0.0.1'],
    { stdio: 'ignore' },
  );
  const heads: string[] = [];
  const server = https.createServer({ key: fs.readFileSync(key), cert: fs.readFileSync(cert) }, (req, res) => {
    heads.push(JSON.stringify(req.headers));
    const up = http.request(
      { host: upstream.hostname, port: upstream.port, path: req.url, method: req.method, headers: req.headers },
      (r) => {
        res.writeHead(r.statusCode ?? 502, r.headers);
        r.pipe(res);
      },
    );
    up.on('error', () => res.destroy());
    req.pipe(up);
  });
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  const port = (server.address() as net.AddressInfo).port;
  return {
    url: `https://127.0.0.1:${port}`,
    heads,
    close: () =>
      new Promise<void>((resolve) => {
        server.close(() => resolve());
        server.closeAllConnections();
        fs.rmSync(dir, { recursive: true, force: true });
      }),
  };
}
