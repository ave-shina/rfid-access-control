import { NextRequest, NextResponse } from "next/server";

const BACKEND_URL = process.env.BACKEND_URL || "http://localhost:8080";

export async function GET(request: NextRequest) {
  const limit = request.nextUrl.searchParams.get("limit") || "50";
  const res = await fetch(`${BACKEND_URL}/api/logs?limit=${limit}`);
  const data = await res.json();
  return NextResponse.json(data, { status: res.status });
}

export async function DELETE() {
  const res = await fetch(`${BACKEND_URL}/api/logs`, { method: "DELETE" });
  return new NextResponse(null, { status: res.status });
}
