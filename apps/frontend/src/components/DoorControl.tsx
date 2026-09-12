"use client";

import { useState } from "react";
import { fetchAPI } from "../lib/websocket";
import { Button } from "@/components/ui/button";

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
      <Button
        variant="default"
        onClick={() => override(1)}
        disabled={loading}
      >
        [ GRANT ]
      </Button>
      <Button
        variant="destructive"
        onClick={() => override(0)}
        disabled={loading}
      >
        [ DENY ]
      </Button>
    </div>
  );
}
