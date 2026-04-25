import type { Metadata } from "next";
import { JetBrains_Mono } from "next/font/google";
import "./globals.css";
import Link from "next/link";
import { Separator } from "@/components/ui/separator";

const jetbrainsMono = JetBrains_Mono({
  subsets: ["latin"],
  variable: "--font-jetbrains-mono",
});

export const metadata: Metadata = {
  title: "RFID // ACCESS CTRL",
  description: "Real-time RFID access control system",
};

export default function RootLayout({
  children,
}: Readonly<{
  children: React.ReactNode;
}>) {
  return (
    <html lang="en">
      <body className={`${jetbrainsMono.variable} font-mono antialiased`}>
        <div className="flex min-h-screen">
          {/* Sidebar */}
          <nav className="w-60 flex flex-col border-r bg-card">
            <div className="p-4">
              <div className="font-bold text-lg uppercase tracking-[0.3em]">
                RFID
              </div>
              <div className="text-xs text-muted-foreground uppercase tracking-[0.2em] mt-1">
                {"// ACCESS CTRL"}
              </div>
            </div>
            <Separator />
            <div className="flex flex-col p-2 gap-1 flex-1">
              <Link
                href="/"
                className="px-4 py-3 text-sm uppercase tracking-widest font-bold hover:bg-foreground hover:text-background transition-colors"
              >
                Dashboard
              </Link>
              <Link
                href="/users"
                className="px-4 py-3 text-sm uppercase tracking-widest font-bold hover:bg-foreground hover:text-background transition-colors"
              >
                Users
              </Link>
            </div>
            <Separator />
            <div className="p-4">
              <div className="text-[10px] text-muted-foreground uppercase tracking-[0.2em]">
                SYS v0.1.0
              </div>
            </div>
          </nav>

          {/* Main content */}
          <main className="flex-1 p-6">{children}</main>
        </div>
      </body>
    </html>
  );
}
