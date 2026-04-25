import type { Metadata } from "next";
import localFont from "next/font/local";
import "./globals.css";
import Link from "next/link";

const geistSans = localFont({
  src: "./fonts/GeistVF.woff",
  variable: "--font-geist-sans",
  weight: "100 900",
});

export const metadata: Metadata = {
  title: "RFID Access Control",
  description: "Real-time RFID access control dashboard",
};

export default function RootLayout({
  children,
}: Readonly<{
  children: React.ReactNode;
}>) {
  return (
    <html lang="en">
      <body className={`${geistSans.variable} antialiased`}>
        <div className="flex min-h-screen">
          {/* Sidebar */}
          <nav className="w-56 bg-gray-900 text-white flex flex-col">
            <div className="p-4 text-lg font-bold border-b border-gray-700">
              RFID Access
            </div>
            <Link
              href="/"
              className="px-4 py-3 hover:bg-gray-800 transition-colors"
            >
              Dashboard
            </Link>
            <Link
              href="/users"
              className="px-4 py-3 hover:bg-gray-800 transition-colors"
            >
              Users
            </Link>
          </nav>

          {/* Main content */}
          <main className="flex-1 p-6">{children}</main>
        </div>
      </body>
    </html>
  );
}
