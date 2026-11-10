import { pipe } from 'it-pipe'
import { logger, enable } from '@libp2p/logger'
import { voyagerProtocol } from './protocol.js'
import { handleRequest } from './handle-request.js'
import Authorization, { Access } from './authorization.js'
import { EventEmitter } from 'node:events'


export default async ({ orbitdb, defaultAccess, verbose } = {}) => {
  const log = logger('voyager:host')

  if (verbose > 0) {
    enable('voyager:host' + (verbose > 1 ? '*' : ':error'))
  }

  log('start voyager')

  defaultAccess = defaultAccess || Access.DENY

  log('default access:', defaultAccess === Access.ALLOW ? 'allow all' : 'deny all')

  const databases = await orbitdb.open('databases', { type: 'keyvalue' })

  const auth = await Authorization({ orbitdb, defaultAccess })

  const MAX_OPEN_DBS = 200;
  const activeDbs = [];
  const lastActivity = new Map();

  const registerActiveDb = async (db) => {
    const addr = db.address.toString();
    lastActivity.set(addr, Date.now());

    if (!activeDbs.find(d => d.address.toString() === addr)) {
       // EVICTION LOGIC: If too many DBs are open, close the oldest one
       if (activeDbs.length >= MAX_OPEN_DBS) {
          const activeEntry = [...lastActivity.entries()]
            .filter(([a]) => activeDbs.some(d => d.address.toString() === a))
            .sort(([, t1], [, t2]) => t1 - t2)[0];

          if (activeEntry) {
              const oldestAddr = activeEntry[0];
              const idx = activeDbs.findIndex(d => d.address.toString() === oldestAddr);
              if (idx !== -1) {
                 const dbToClose = activeDbs[idx];
                 console.log(`[LRU] Evicting idle database to save RAM: ${oldestAddr}`);
                 activeDbs.splice(idx, 1);
                 lastActivity.delete(oldestAddr);
                 await dbToClose.close().catch(() => {});
              }
          }
       }
       activeDbs.push(db);
       
       // Track activity to prevent eviction of busy databases
       db.events.on('replicated', () => lastActivity.set(addr, Date.now()));
       db.events.on('update', () => lastActivity.set(addr, Date.now()));
    }
  };

  const host = new EventEmitter()
  Object.assign(host, {
    databases,
    activeDbs,
    orbitdb,
    auth,
    registerActiveDb,
    log
  })

  const handleMessages = async ({ stream }) => {
    await pipe(stream, handleRequest(host), stream)
  }

  await orbitdb.ipfs.libp2p.handle(voyagerProtocol, handleMessages, { runOnLimitedConnection: true })

  log('open replicated databases')
  
  let count = 0
  for await (const db of databases.iterator()) {
    console.log(`[BOOT] Opening database: ${db.key}...`)
    try {
        const opened = await orbitdb.open(db.key, { sync: true })
        console.log(`[BOOT] Opened! Address: ${opened.address.toString()} | Log ID: ${opened.log.id} | Heads: ${(await opened.log.heads()).length}`)
        
        opened.events.on('replicated', (address, heads) => {
            console.log(`[SYNC] 📥 NEW DATA for ${address} | New Heads: ${heads.length} | Total: ${(opened.log.heads().length)}`)
        })
        
        opened.events.on('update', (entry) => {
            console.log(`[SYNC] 📝 Local update/append for ${db.key}`)
        })

        await host.registerActiveDb(opened)
        host.emit('pinning:database', opened)
        count++
    } catch (e) {
        console.error(`[BOOT] FAILED to open ${db.key}: ${e.message}`)
    }
  }

  log(count, 'databases opened')

  const stop = async () => {
    await orbitdb.ipfs.libp2p.unhandle(voyagerProtocol)
  }

  host.stop = stop
  return host
}
