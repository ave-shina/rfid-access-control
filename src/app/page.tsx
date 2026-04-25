import LiveLogTable from "../components/LiveLogTable";
import DoorControl from "../components/DoorControl";

export default function DashboardPage() {
  return (
    <div>
      <div className="flex items-center justify-between mb-6">
        <h1 className="text-2xl font-bold">Dashboard</h1>
        <DoorControl />
      </div>

      <h2 className="text-lg font-semibold mb-3">Live Access Log</h2>
      <LiveLogTable />
    </div>
  );
}
