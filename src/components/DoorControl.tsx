"use client";

import { useState } from "react";
import { fetchAPI } from "../lib/websocket";

export default function DoorControl() {
  const [loading, setLoading] = useState(false);

  const override = async (status: number) => {
    const action = status === 1 ? "GRANT" : "DENY";
    if (!confirm(`Are you sure you want to ${action} access?`)) return;

    setLoading(true);
    try {
      await fetchAPI("/api/door/override", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          status,
          message: `Manual Override — ${action === "GRANT" ? "Access Granted" : "Access Denied"}`,
          action_by: "Dashboard",
        }),
      });
    } catch (err) {
      alert(`Override failed: ${err}`);
    } finally {
      setLoading(false);
    }
  };

  return (
    <div className="flex gap-3">
      <button
        onClick={() => override(1)}
        disabled={loading}
        className="px-4 py-2 bg-green-600 text-white rounded hover:bg-green-700 disabled:opacity-50 transition-colors"
      >
        Grant Access
      </button>
      <button
        onClick={() => override(0)}
        disabled={loading}
        className="px-4 py-2 bg-red-600 text-white rounded hover:bg-red-700 disabled:opacity-50 transition-colors"
      >
        Deny Access
      </button>
    </div>
  );
}
