"use client";

import { useEffect, useState } from "react";
import { useWebSocket, fetchAPI, type WSMessage } from "../lib/websocket";
import {
  Table,
  TableHeader,
  TableBody,
  TableRow,
  TableHead,
  TableCell,
} from "@/components/ui/table";
import { Badge } from "@/components/ui/badge";

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
      <div className="mb-4 flex items-center gap-3 mt-2">
        <span className="text-xs text-muted-foreground uppercase tracking-widest">
          Device Status:
        </span>
        <span
          className={`inline-block w-3 h-3 border-2 border-foreground ${
            deviceStatus === "online"
              ? "bg-foreground"
              : deviceStatus === "offline"
                ? "bg-transparent"
                : "bg-muted-foreground"
          }`}
        />
        <span className="text-sm font-bold uppercase tracking-widest">
          {deviceStatus}
        </span>
      </div>

      {/* Log Table */}
      <div className="border">
        <Table>
          <TableHeader>
            <TableRow>
              <TableHead className="uppercase tracking-wider text-xs">
                Time
              </TableHead>
              <TableHead className="uppercase tracking-wider text-xs">
                Status
              </TableHead>
              <TableHead className="uppercase tracking-wider text-xs">
                User
              </TableHead>
              <TableHead className="uppercase tracking-wider text-xs">
                Device
              </TableHead>
              <TableHead className="uppercase tracking-wider text-xs">
                UID Hash
              </TableHead>
            </TableRow>
          </TableHeader>
          <TableBody>
            {logs.map((log) => (
              <TableRow key={log.id}>
                <TableCell className="text-muted-foreground font-mono text-xs">
                  {new Date(log.timestamp).toLocaleTimeString()}
                </TableCell>
                <TableCell>
                  <Badge
                    variant={
                      log.status === "AUTHORIZED" ? "default" : "destructive"
                    }
                  >
                    {log.status}
                  </Badge>
                </TableCell>
                <TableCell className="font-bold">{log.action_by}</TableCell>
                <TableCell className="text-muted-foreground font-mono text-xs">
                  {log.device_id}
                </TableCell>
                <TableCell className="font-mono text-xs text-muted-foreground">
                  {log.rfid_uid_hash.slice(0, 16)}...
                </TableCell>
              </TableRow>
            ))}
            {logs.length === 0 && (
              <TableRow>
                <TableCell
                  colSpan={5}
                  className="text-center text-muted-foreground py-8 uppercase tracking-widest text-xs"
                >
                  {"// No access logs yet"}
                </TableCell>
              </TableRow>
            )}
          </TableBody>
        </Table>
      </div>
    </div>
  );
}
