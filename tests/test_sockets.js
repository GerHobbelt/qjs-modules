import * as os from 'os';
import * as std from 'std';
import inspect from 'inspect';
import { socklen_t, fd_set, SockAddr, Socket, select, poll, AF_INET, SOCK_STREAM, IPPROTO_TCP, POLLIN, POLLOUT, POLLERR, POLLHUP, O_NONBLOCK, O_ASYNC, SO_ERROR, SO_DEBUG, SO_REUSEPORT, SO_REUSEADDR, SO_KEEPALIVE, SO_DONTROUTE, SO_BROADCAST, SO_OOBINLINE, SO_SNDBUF, SO_RCVBUF, SOL_SOCKET } from 'sockets';
import { error, escape, quote, toString, toArrayBuffer, randi, randf, srand } from 'misc';
import { define } from 'util';
//import { SyscallError, EAGAIN, ENETDOWN, EPROTO, ENOPROTOOPT, EHOSTUNREACH, EOPNOTSUPP, ENETUNREACH, EBADF, ECONNABORTED, EINVAL, ENOBUFS, ENOTSOCK, ESOCKTNOSUPPORT, EPROTONOSUPPORT, ETIMEDOUT, EADDRINUSE, EADDRNOTAVAIL, EIO, EAFNOSUPPORT, EALREADY, ECONNREFUSED, EINPROGRESS, EISCONN, EPROTOTYPE, ENOTCONN, EMSGSIZE, ECONNRESET, EDESTADDRREQ } from 'syscallerror';
import Console from 'console';
import fs from 'fs';

function main() {
  globalThis.console = new Console({
    inspectOptions: {
      colors: true,
      depth: 4,
      breakLength: 80,
      maxArrayLength: 100,
      maxStringLength: 100,
      compact: false
    }
  });
  let seed = +Date.now();
  srand(seed);

  let la = new SockAddr(AF_INET, new Uint8Array([192, 168, 8, 151]).buffer, 31337);
  la = new SockAddr(AF_INET, '0.0.0.0', randi() & 0xffff);
  let ra = new SockAddr(AF_INET, '192.168.8.156', 22);
  console.log(`classes`, { socklen_t, fd_set, SockAddr, Socket });
  console.log(`la.clone() =`, la.clone());
  console.log(`la.toString() =`, la.toString());
  console.log(`ra.toString() =`, ra.toString());
  let opt, ret, data, timeout, pfds, sock;
  sock = new Socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  console.log('sock', sock);

  sock.getsockopt(SOL_SOCKET, SO_REUSEADDR, (opt = []), 4);
  console.log(`SOL_SOCKET =`, SOL_SOCKET);
  console.log(`SO_REUSEADDR =`, SO_REUSEADDR);
  console.log(`opt =`, opt);
  sock.setsockopt(SOL_SOCKET, SO_REUSEADDR, [1]);
  sock.setsockopt(SOL_SOCKET, SO_REUSEPORT, [1]);
  sock.setsockopt(SOL_SOCKET, SO_DEBUG, [1]);

  if(true) {
    const so_flags = Object.entries({
      SO_ERROR,
      SO_DEBUG,
      SO_REUSEPORT,
      SO_REUSEADDR,
      SO_KEEPALIVE,
      SO_DONTROUTE,
      SO_BROADCAST,
      SO_OOBINLINE,
      SO_SNDBUF,
      SO_RCVBUF
    })
      .sort((a, b) => a[1] - b[1])
      .sort((a, b) => a[0].localeCompare(b[0]));

    for(let [name, value] of so_flags) {
      sock.getsockopt(SOL_SOCKET, value, (opt = [0]), 4);
      console.log(`[${value.toString()}]`.padStart(4), `${name.padEnd(30)} =`, opt);
    }
  }
  console.log(`ndelay() =`, sock.ndelay());
  console.log(`bind(`, la, `) =`, sock.bind(la));
  console.log(`connect(`, ra, `) =`, sock.connect(ra), sock.error);

  function DumpSock(s) {
    let { fd, errno, syscall, error, local, remote, open, eof, mode } = s;

    return [inspect({ fd, errno, syscall, error, /*open, eof,*/ mode }, { colors: true })];

    //  return [s.fd >= 0 && `fd=${s.fd}`, s.eof ? 'EOF' : s.open ? 'open' : 'closed', s.ret !== undefined && `r=${s.ret}`, s.syscall !== undefined && `f=${s.syscall}`,  'e=', s.errno/*,  `e=${s.err}`*/].filter(p => !!p);
  }

  function ioFlags(flags = 0) {
    let o = [];
    if(flags & POLLIN) o.push('IN');
    if(flags & POLLOUT) o.push('OUT');
    if(flags & POLLERR) o.push('ERR');
    if(flags & POLLHUP) o.push('HUP');
    return o.join('|');
  }

  function PollFD(fd, events = POLLIN) {
    this.fd = fd;
    this.events = events;
    this.revents = 0;
    return this;
  }

  define(PollFD.prototype, {
    [Symbol.inspect]() {
      const { fd, events, revents } = this;
      return `{ fd: ${fd}, events: ${ioFlags(events)}, revents: ${ioFlags(revents)} }`;
    },
    inspect() {
      const { fd, events, revents } = this;
      return `{ fd: ${fd}, events: ${ioFlags(events)}, revents: ${ioFlags(revents)} }`;
    }
  });

  function waitIO(flags = POLLIN) {
    ret = poll((pfds = [new PollFD(sock.fd, flags | POLLERR)]), pfds.length, (timeout = 3000));

    // console.log(`poll(${pfds.map(pfd => pfd.inspect()).join(', ')}, ${pfds.length}, ${timeout}) =`, ret);
    //console.log(`poll() =`, ret, ioFlags(pfds[0].revents));
  }

  waitIO(POLLOUT);

  /*  let rfds = [],
    wfds = [sock.fd],
    efds = [];
    timeout = [10, 0];

  console.log('select', { rfds, wfds, efds, timeout });
  console.log('select(sock.fd + 1, rfds, wfds, efds, timeout) = ', select(sock.fd + 1, rfds, wfds, efds, timeout));
  console.log('select', { rfds, wfds, efds, timeout });

  select(sock.fd + 1, [sock.fd], null, null, timeout);
*/
  console.log('local =', sock.local);
  console.log('remote =', sock.remote);

  let n,
    buf = new ArrayBuffer(1024);

  for(;;) {
    waitIO(POLLIN);

    if(pfds[0].revents & POLLHUP || n == 0) {
      break;
    }

    if(pfds[0].revents & POLLIN) {
      data = toString(buf, 0, (n = sock.recv(buf)));
      console.log(
        (
          `recv(ArrayBuffer ${buf.byteLength})` +
          /*inspect(buf, {colors: true, maxArrayLength: 4, maxStringLength: 10, multiline: false, breakLength: Infinity, compact: 2 }).replace(/\s+/g, ' '),*/
          ' = ' +
          n +
          (n >= 0 ? quote(data, "'") : sock.error + '')
        ).padEnd(70),
        ...DumpSock(sock)
      );

      if(data.indexOf('OpenSSH') != -1) {
        const txt = 'BLAHBLAHTEST\r\n';
        let start = 4;
        n = sock.send(txt, start);
        console.log(`send(${quote(txt.slice(start), "'")}, ${start}) =`, n, n > 0 ? null : sock.error, ...DumpSock(sock));
      }
    }
  }

  let { open, error } = sock;
  if(error) console.log('error:', error);
  console.log('O_NONBLOCK', O_NONBLOCK.toString(2).padStart(16, '0'));
  console.log('O_ASYNC   ', O_ASYNC.toString(2).padStart(16, '0'));
  console.log('sock.mode ', sock.mode.toString(2).padStart(16, '0'));

  console.log(('sock.close() ' + sock.close() + '').padEnd(70), ...DumpSock(sock));
}

main();