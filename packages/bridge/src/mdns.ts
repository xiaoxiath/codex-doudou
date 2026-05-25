/**
 * mDNS / DNS-SD announce so real Doudou devices (and the simulator from
 * other LAN hosts) can find this Bridge without hard-coded IPs.
 *
 * Service:    _doudou._tcp.local
 * Instance:   "Doudou Bridge"
 * Port:       Bridge HTTP/WS port
 * TXT:        v=<protoVer>, path=/device, sim=<simulator url>
 */
import { Bonjour, type Service } from 'bonjour-service';

import { log } from './log.js';

export interface MdnsAnnouncer {
  stop(): Promise<void>;
}

export function announceBridge(port: number, protocolVersion = 1): MdnsAnnouncer {
  let instance: Bonjour | null = null;
  let service: Service | null = null;
  try {
    instance = new Bonjour();
    service = instance.publish({
      name: 'Doudou Bridge',
      type: 'doudou',          // _doudou._tcp.local
      protocol: 'tcp',
      port,
      txt: {
        v: String(protocolVersion),
        path: '/device',
        sim: `/`,
      },
    });
    log.info({ port, type: '_doudou._tcp.local' }, 'mDNS announcing bridge');
  } catch (err) {
    log.warn({ err: String(err) }, 'mDNS announce failed (not fatal)');
  }
  return {
    async stop() {
      if (service?.stop) {
        try { service.stop(() => undefined); } catch { /* ignore */ }
      }
      if (instance) {
        try { instance.destroy(); } catch { /* ignore */ }
      }
      log.info('mDNS stopped');
    },
  };
}
