import LiveLogTable from "../components/LiveLogTable";
import DoorControl from "../components/DoorControl";
import {
  Card,
  CardHeader,
  CardTitle,
  CardDescription,
  CardContent,
} from "@/components/ui/card";

export default function DashboardPage() {
  return (
    <div className="space-y-6">
      <div className="flex items-center justify-between">
        <div>
          <h1 className="text-2xl font-bold uppercase tracking-[0.3em]">
            Dashboard
          </h1>
          <p className="text-sm text-muted-foreground uppercase tracking-widest mt-1">
            {"// Real-time access monitor"}
          </p>
        </div>
        <DoorControl />
      </div>

      <Card>
        <CardHeader>
          <CardTitle className="uppercase tracking-wider">
            Live Access Log
          </CardTitle>
          <CardDescription className="uppercase tracking-widest">
            Recent scan events from all devices
          </CardDescription>
        </CardHeader>
        <CardContent>
          <LiveLogTable />
        </CardContent>
      </Card>
    </div>
  );
}
