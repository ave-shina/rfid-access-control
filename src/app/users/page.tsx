"use client";

import { useEffect, useState } from "react";
import { fetchAPI } from "../../lib/websocket";

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
    <div>
      <h1 className="text-2xl font-bold mb-6">Users</h1>

      {/* Add User Form */}
      <form onSubmit={addUser} className="mb-8 p-4 border rounded-lg bg-gray-50">
        <h2 className="text-lg font-semibold mb-3">Add User</h2>
        {error && (
          <p className="mb-3 text-sm text-red-600 bg-red-50 px-3 py-2 rounded">
            {error}
          </p>
        )}
        <div className="flex flex-wrap gap-3">
          <input
            type="text"
            placeholder="Name"
            value={name}
            onChange={(e) => setName(e.target.value)}
            required
            className="px-3 py-2 border rounded text-sm flex-1 min-w-[150px]"
          />
          <input
            type="text"
            placeholder="RFID UID (hex)"
            value={rfidUid}
            onChange={(e) => setRfidUid(e.target.value)}
            required
            className="px-3 py-2 border rounded text-sm flex-1 min-w-[150px] font-mono"
          />
          <select
            value={role}
            onChange={(e) => setRole(e.target.value)}
            className="px-3 py-2 border rounded text-sm"
          >
            <option value="employee">Employee</option>
            <option value="admin">Admin</option>
            <option value="contractor">Contractor</option>
          </select>
          <button
            type="submit"
            disabled={adding}
            className="px-4 py-2 bg-blue-600 text-white rounded hover:bg-blue-700 disabled:opacity-50 text-sm"
          >
            {adding ? "Adding..." : "Add User"}
          </button>
        </div>
      </form>

      {/* Users Table */}
      <div className="overflow-auto border rounded-lg">
        <table className="w-full text-sm">
          <thead className="bg-gray-100">
            <tr>
              <th className="px-4 py-2 text-left">Name</th>
              <th className="px-4 py-2 text-left">Role</th>
              <th className="px-4 py-2 text-left">Status</th>
              <th className="px-4 py-2 text-left">UID Hash</th>
              <th className="px-4 py-2 text-left">Created</th>
              <th className="px-4 py-2 text-left">Actions</th>
            </tr>
          </thead>
          <tbody>
            {users.map((user) => (
              <tr key={user.id} className="border-t hover:bg-gray-50">
                <td className="px-4 py-2 font-medium">{user.name}</td>
                <td className="px-4 py-2 capitalize">{user.role}</td>
                <td className="px-4 py-2">
                  <span
                    className={`px-2 py-0.5 rounded text-xs font-semibold ${
                      user.is_active
                        ? "bg-green-100 text-green-800"
                        : "bg-gray-200 text-gray-600"
                    }`}
                  >
                    {user.is_active ? "Active" : "Inactive"}
                  </span>
                </td>
                <td className="px-4 py-2 font-mono text-xs text-gray-400">
                  {user.rfid_uid_hash.slice(0, 16)}...
                </td>
                <td className="px-4 py-2 text-gray-500">
                  {new Date(user.created_at).toLocaleDateString()}
                </td>
                <td className="px-4 py-2">
                  <div className="flex gap-2">
                    <button
                      onClick={() => toggleActive(user)}
                      className={`px-2 py-1 rounded text-xs ${
                        user.is_active
                          ? "bg-yellow-100 text-yellow-800 hover:bg-yellow-200"
                          : "bg-green-100 text-green-800 hover:bg-green-200"
                      }`}
                    >
                      {user.is_active ? "Deactivate" : "Activate"}
                    </button>
                    <button
                      onClick={() => deleteUser(user)}
                      className="px-2 py-1 rounded text-xs bg-red-100 text-red-800 hover:bg-red-200"
                    >
                      Delete
                    </button>
                  </div>
                </td>
              </tr>
            ))}
            {users.length === 0 && (
              <tr>
                <td
                  colSpan={6}
                  className="px-4 py-8 text-center text-gray-400"
                >
                  No users registered
                </td>
              </tr>
            )}
          </tbody>
        </table>
      </div>
    </div>
  );
}
