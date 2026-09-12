"use client";

import { useEffect, useState } from "react";
import { fetchAPI } from "../../lib/websocket";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import {
  Card,
  CardHeader,
  CardTitle,
  CardDescription,
  CardContent,
} from "@/components/ui/card";
import {
  Table,
  TableHeader,
  TableBody,
  TableRow,
  TableHead,
  TableCell,
} from "@/components/ui/table";
import { Badge } from "@/components/ui/badge";

interface User {
  id: number;
  name: string;
  rfid_uid_hash: string;
  role: string;
  is_active: boolean;
  created_at: string;
}

export default function UsersPage() {
  const [users, setUsers] = useState<User[]>([]);
  const [name, setName] = useState("");
  const [rfidUid, setRfidUid] = useState("");
  const [role, setRole] = useState("employee");
  const [error, setError] = useState("");
  const [adding, setAdding] = useState(false);

  const loadUsers = () => {
    fetchAPI<User[]>("/api/users")
      .then((data) => setUsers(data || []))
      .catch(() => setError("Failed to load users"));
  };

  useEffect(loadUsers, []);

  const addUser = async (e: React.FormEvent) => {
    e.preventDefault();
    setError("");
    setAdding(true);
    try {
      await fetchAPI("/api/users", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ name, rfid_uid: rfidUid, role }),
      });
      setName("");
      setRfidUid("");
      setRole("employee");
      loadUsers();
    } catch (err) {
      setError(String(err));
    } finally {
      setAdding(false);
    }
  };

  const toggleActive = async (user: User) => {
    try {
      await fetchAPI(`/api/users/${user.id}`, {
        method: "PATCH",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ is_active: !user.is_active }),
      });
      loadUsers();
    } catch (err) {
      setError(String(err));
    }
  };

  const deleteUser = async (user: User) => {
    if (!confirm(`Delete user "${user.name}"?`)) return;
    try {
      await fetchAPI(`/api/users/${user.id}`, { method: "DELETE" });
      loadUsers();
    } catch (err) {
      setError(String(err));
    }
  };

  return (
    <div className="space-y-6">
      <div>
        <h1 className="text-2xl font-bold uppercase tracking-[0.3em]">
          Users
        </h1>
        <p className="text-sm text-muted-foreground uppercase tracking-widest mt-1">
          {"// Credential management"}
        </p>
      </div>

      <Card>
        <CardHeader>
          <CardTitle className="uppercase tracking-wider">Add User</CardTitle>
          <CardDescription className="uppercase tracking-widest">
            Register a new RFID credential
          </CardDescription>
        </CardHeader>
        <CardContent>
          <form onSubmit={addUser} className="space-y-4">
            {error && (
              <div className="border bg-destructive text-destructive-foreground px-4 py-2 text-xs uppercase tracking-widest font-bold">
                ERR: {error}
              </div>
            )}
            <div className="flex flex-wrap gap-3">
              <Input
                type="text"
                placeholder="NAME"
                value={name}
                onChange={(e) => setName(e.target.value)}
                required
                className="flex-1 min-w-[150px]"
              />
              <Input
                type="text"
                placeholder="RFID UID (hex)"
                value={rfidUid}
                onChange={(e) => setRfidUid(e.target.value)}
                required
                className="flex-1 min-w-[150px] font-mono"
              />
              <select
                value={role}
                onChange={(e) => setRole(e.target.value)}
                className="flex h-10 border border-input bg-background px-3 py-2 text-sm font-mono uppercase tracking-wider focus:outline-none focus:ring-2 focus:ring-ring"
              >
                <option value="employee">EMPLOYEE</option>
                <option value="admin">ADMIN</option>
                <option value="contractor">CONTRACTOR</option>
              </select>
              <Button type="submit" disabled={adding}>
                {adding ? "ADDING..." : "[ ADD ]"}
              </Button>
            </div>
          </form>
        </CardContent>
      </Card>

      <div className="border">
        <Table>
          <TableHeader>
            <TableRow>
              <TableHead className="uppercase tracking-wider text-xs">
                Name
              </TableHead>
              <TableHead className="uppercase tracking-wider text-xs">
                Role
              </TableHead>
              <TableHead className="uppercase tracking-wider text-xs">
                Status
              </TableHead>
              <TableHead className="uppercase tracking-wider text-xs">
                UID Hash
              </TableHead>
              <TableHead className="uppercase tracking-wider text-xs">
                Created
              </TableHead>
              <TableHead className="uppercase tracking-wider text-xs">
                Actions
              </TableHead>
            </TableRow>
          </TableHeader>
          <TableBody>
            {users.map((user) => (
              <TableRow key={user.id}>
                <TableCell className="font-bold">{user.name}</TableCell>
                <TableCell className="uppercase tracking-wider text-xs">
                  {user.role}
                </TableCell>
                <TableCell>
                  <Badge variant={user.is_active ? "default" : "outline"}>
                    {user.is_active ? "ACTIVE" : "INACTIVE"}
                  </Badge>
                </TableCell>
                <TableCell className="font-mono text-xs text-muted-foreground">
                  {user.rfid_uid_hash.slice(0, 16)}...
                </TableCell>
                <TableCell className="text-muted-foreground text-xs">
                  {new Date(user.created_at).toLocaleDateString()}
                </TableCell>
                <TableCell>
                  <div className="flex gap-2">
                    <Button
                      variant="outline"
                      size="sm"
                      onClick={() => toggleActive(user)}
                    >
                      {user.is_active ? "DEACTIVATE" : "ACTIVATE"}
                    </Button>
                    <Button
                      variant="destructive"
                      size="sm"
                      onClick={() => deleteUser(user)}
                    >
                      DEL
                    </Button>
                  </div>
                </TableCell>
              </TableRow>
            ))}
            {users.length === 0 && (
              <TableRow>
                <TableCell
                  colSpan={6}
                  className="text-center text-muted-foreground py-8 uppercase tracking-widest text-xs"
                >
                  {"// No users registered"}
                </TableCell>
              </TableRow>
            )}
          </TableBody>
        </Table>
      </div>
    </div>
  );
}
