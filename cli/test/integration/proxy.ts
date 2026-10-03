import net from 'node:net';

/**
 * A TCP proxy in front of the REAL flapi server that, once, forwards a mutating
 * request and then drops the connection WITHOUT relaying the response - the
 * "server committed, answer lost" case that makes a retrying client repeat a
 * write. Counts the requests that reach the server by method.
 */
export async function lossyProxy(upstreamUrl: string) {
  const upstream = new URL(upstreamUrl);
  const seen: string[] = [];
  let dropped = false;

  const server = net.createServer((client) => {
    const up = net.connect(Number(upstream.port), upstream.hostname);
    let first = true;
    let dropping = false;
    client.on('data', (chunk) => {
      if (first) {
        first = false;
        const method = chunk.toString('latin1', 0, 8).split(' ')[0];
        seen.push(method);
        if (method === 'POST' && !dropped) {
          dropped = true;
          dropping = true;
          // Forward it, then cut the client off before ANY answer comes back. The
          // relay below must not forward it first (it raced the destroy, so the
          // client sometimes got the response and the test flaked).
          up.write(chunk);
          up.once('data', () => client.destroy());
          return;
        }
      }
      up.write(chunk);
    });
    up.on('data', (d) => !dropping && client.writable && client.write(d));
    up.on('close', () => client.end());
    client.on('close', () => up.destroy());
    client.on('error', () => up.destroy());
    up.on('error', () => client.destroy());
  });

  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  const port = (server.address() as net.AddressInfo).port;
  return {
    url: `http://127.0.0.1:${port}`,
    count: (method: string) => seen.filter((m) => m === method).length,
    close: () => new Promise<void>((resolve) => server.close(() => resolve())),
  };
}
