"use client";

import { useEffect, useState } from "react";
import { useWebSocket, fetchAPI, type WSMessage } from "../lib/websocket";

interface AccessLog {
  id: number;
  rfid_uid_hash: string;
  status: string;
  action_by: string;
  device_id: string;
  timestamp: string;
}

export default function LiveLogTable() {
  const [logs, setLogs] = useState<AccessLog[]>([]);
  const [deviceStatus, setDeviceStatus] = useState<string>("unknown");

  useEffect(() => {
    fetchAPI<AccessLog[]>("/api/logs?limit=50")
      .then((data) => setLogs(data || []))
      .catch(() => {});
  }, []);

  useWebSocket((msg: WSMessage) => {
    if (msg.type === "access_log") {
      setLogs((prev) => [msg.data as AccessLog, ...prev].slice(0, 100));
    } else if (msg.type === "device_status") {
      const d = msg.data as { status?: string };
      setDeviceStatus(d?.status || "unknown");
    }
  });

  return (
    <div>
      {/* Device Status */}
      <div className="mb-4 flex items-center gap-2">
        <span className="text-sm text-gray-500">Device Status:</span>
        <span
          className={`inline-block w-3 h-3 rounded-full ${
            deviceStatus === "online"
              ? "bg-green-500"
              : deviceStatus === "offline"
              ? "bg-red-500"
              : "bg-gray-400"
          }`}
        />
        <span className="text-sm font-medium">{deviceStatus}</span>
      </div>

      {/* Log Table */}
      <div className="overflow-auto max-h-[600px] border rounded-lg">
        <table className="w-full text-sm">
          <thead className="bg-gray-100 sticky top-0">
            <tr>
              <th className="px-4 py-2 text-left">Time</th>
              <th className="px-4 py-2 text-left">Status</th>
              <th className="px-4 py-2 text-left">User</th>
              <th className="px-4 py-2 text-left">Device</th>
              <th className="px-4 py-2 text-left">UID Hash</th>
            </tr>
          </thead>
          <tbody>
            {logs.map((log) => (
              <tr key={log.id} className="border-t hover:bg-gray-50">
                <td className="px-4 py-2 text-gray-500">
                  {new Date(log.timestamp).toLocaleTimeString()}
                </td>
                <td className="px-4 py-2">
                  <span
                    className={`px-2 py-0.5 rounded text-xs font-semibold ${
                      log.status === "AUTHORIZED"
                        ? "bg-green-100 text-green-800"
                        : "bg-red-100 text-red-800"
                    }`}
                  >
                    {log.status}
                  </span>
                </td>
                <td className="px-4 py-2">{log.action_by}</td>
                <td className="px-4 py-2 text-gray-500">{log.device_id}</td>
                <td className="px-4 py-2 font-mono text-xs text-gray-400">
                  {log.rfid_uid_hash.slice(0, 16)}...
                </td>
              </tr>
            ))}
            {logs.length === 0 && (
              <tr>
                <td colSpan={5} className="px-4 py-8 text-center text-gray-400">
                  No access logs yet
                </td>
              </tr>
            )}
          </tbody>
        </table>
      </div>
    </div>
  );
}
