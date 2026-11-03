// ABOUTME: What the host can learn from their own PC about friends reaching wotm_server over the internet: the public address from a
// ABOUTME: STUN server, its verdict against the router's UPnP address, and a TCP self-test through the router. Pure parts tested by LobbyTest.cs.
using System;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;

// RFC 5389 binding request/response, just enough to learn the address the internet sees (IPv4).
static class Stun
{
    public static readonly string[] Servers = { "stun.l.google.com:19302", "stun.cloudflare.com:3478", "stun1.l.google.com:19302" };
    static readonly byte[] Cookie = { 0x21, 0x12, 0xA4, 0x42 };

    // type 0x0001 (binding request), length 0, magic cookie, 12-byte transaction id
    public static byte[] Request(byte[] txn)
    {
        var r = new byte[20];
        r[1] = 1;
        Array.Copy(Cookie, 0, r, 4, 4);
        Array.Copy(txn, 0, r, 8, 12);
        return r;
    }

    // The IPv4 address in the binding success response to the request with transaction id txn: XOR-MAPPED-ADDRESS (0x0020) first,
    // MAPPED-ADDRESS (0x0001) otherwise. null for anything else (error response, another transaction, no IPv4 address, truncated).
    public static IPAddress ParseMappedAddress(byte[] reply, byte[] txn)
    {
        if (reply == null || reply.Length < 20 || reply[0] != 0x01 || reply[1] != 0x01) return null;
        for (int i = 0; i < 4; ++i) if (reply[4 + i] != Cookie[i]) return null;
        for (int i = 0; i < 12; ++i) if (reply[8 + i] != txn[i]) return null;
        int end = Math.Min(reply.Length, 20 + ((reply[2] << 8) | reply[3]));
        IPAddress plain = null;
        for (int i = 20; i + 4 <= end; )
        {
            int type = (reply[i] << 8) | reply[i + 1], len = (reply[i + 2] << 8) | reply[i + 3], v = i + 4;
            if (v + len > end) break;
            if ((type == 0x0020 || type == 0x0001) && len >= 8 && reply[v + 1] == 1)   // family 1 = IPv4
            {
                var b = new byte[4];
                Array.Copy(reply, v + 4, b, 0, 4);
                if (type == 0x0020)
                {
                    for (int k = 0; k < 4; ++k) b[k] ^= Cookie[k];
                    return new IPAddress(b);
                }
                if (plain == null) plain = new IPAddress(b);
            }
            i = v + len + (4 - len % 4) % 4;
        }
        return plain;
    }
}
