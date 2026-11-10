/**
 * Cloud-assisted local network device discovery.
 * Queries the gateway endpoint /api/devices/nearby to find OnMyDisk
 * storage nodes operating within the same public IP / LAN.
 */

export async function fetchNearbyDevices(gatewayUrl = "") {
  try {
    let baseUrl = gatewayUrl;
    if (!baseUrl) {
      if (typeof window !== "undefined") {
        if (window.location.hostname === "localhost" || window.location.hostname === "127.0.0.1") {
          baseUrl = "https://direct.onmydisk.net";
        } else {
          baseUrl = window.location.origin;
        }
      } else {
        baseUrl = "https://direct.onmydisk.net";
      }
    }

    const res = await fetch(`${baseUrl}/api/devices/nearby`, {
      method: "GET",
      headers: {
        "Accept": "application/json"
      }
    });

    if (!res.ok) {
      return [];
    }

    const data = await res.json();
    if (data && Array.isArray(data.devices)) {
      return data.devices.map(d => {
        // Short 10-char LinkID is the deviceId/user from client certificate
        let shortId = d.deviceId || "";
        if (!shortId && d.user && typeof d.user === "string" && d.user.length <= 16) {
          shortId = d.user;
        }
        if (!shortId) {
          shortId = d.linkId || "";
        }
        return {
          ...d,
          linkId: shortId
        };
      });
    }
    return [];
  } catch (err) {
    console.warn("[Discovery] fetchNearbyDevices error:", err);
    return [];
  }
}
