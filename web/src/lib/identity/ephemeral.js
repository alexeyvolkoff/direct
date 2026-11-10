import Gun from "gun/gun";
import "gun/sea";

/**
 * Ephemeral identity — current behavior, no changes.
 * Device connects via LinkID only, no GunDB user graph.
 *
 * Provides a thin wrapper so App.svelte can treat
 * ephemeral and persistent identities uniformly.
 */
export class EphemeralIdentity {
  constructor() {
    /** @type {"ephemeral"} */
    this.type = "ephemeral";
    /** @type {null} */
    this.gun = null;
    /** @type {null} */
    this.mainPub = null;
    /** @type {null} */
    this.did = null;
  }

  async init() {
    console.log("[identity] ephemeral mode — no persistent identity");
    return this;
  }

  /** No-op: ephemeral mode has no device list to subscribe to. */
  subscribeDeviceList() {
    return () => {};
  }

  /** No-op: ephemeral mode cannot log out (nothing to clear). */
  async logout() {}

  destroy() {
    this.gun = null;
  }
}
