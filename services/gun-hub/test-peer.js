import fs from 'fs'
import { createFromJSON, createEd25519PeerId } from '@libp2p/peer-id-factory'

const PEER_ID_FILE = './peer-id.json'

let peerId;
if (fs.existsSync(PEER_ID_FILE)) {
  console.log(`Loading existing Peer ID from ${PEER_ID_FILE}...`)
  const peerIdJson = JSON.parse(fs.readFileSync(PEER_ID_FILE, 'utf8'))
  peerId = await createFromJSON(peerIdJson)
} else {
  console.log('Generating new Ed25519 Peer ID...')
  peerId = await createEd25519PeerId()
  fs.writeFileSync(PEER_ID_FILE, JSON.stringify({
    id: peerId.toString(),
    privKey: Buffer.from(peerId.privateKey).toString('base64'),
    pubKey: Buffer.from(peerId.publicKey).toString('base64')
  }, null, 2))
  console.log(`Saved new Peer ID to ${PEER_ID_FILE}`)
}
console.log(peerId.toString());
