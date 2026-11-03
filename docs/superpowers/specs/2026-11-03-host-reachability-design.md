# Host reachability: public address and port self-test

Date: 2026-11-03. Branch: host-reachability (on top of PR #1, the Windows Firewall check). Status: approved by Jake, written for Lane's review.

## Goal

A friend typed the host's public address and port and got "no answer", with the host's box saying the router had opened the port.
UPnP cannot see two things: whether the address the router reports is the one the internet sees (a second router or the provider's
shared address, CGNAT, in between), and whether anything actually arrives at the port. The host should learn both from their own PC,
in the share box, with no server of ours and no friend involved.

Launcher only (`tools/launcher`), no change to `wotm_server`, no infrastructure. The master server's connect-back stays a possible
follow-up once somebody runs a master.

## Section 1: the public address, from STUN

- `Stun.cs`: one binding request (RFC 5389, 20-byte header, random transaction id) to each of three public servers in parallel
  (`stun.l.google.com:19302`, `stun.cloudflare.com:3478`, `stun1.l.google.com:19302`); the first valid answer wins, 1.5 s timeout,
  one UDP packet each. Measured from Jake's PC: all three answer in under 150 ms with the real public address.
- Pure parser `Stun.ParseMappedAddress(byte[] reply, byte[] txn)`: checks the message type (binding success 0x0101), magic cookie and
  transaction id, walks the attributes with 4-byte padding, prefers XOR-MAPPED-ADDRESS (0x0020, un-XORed with the cookie) and accepts
  MAPPED-ADDRESS (0x0001); IPv4 only; null on anything else.
- Live query `Stun.PublicAddress(int timeoutMs)` returns the address or null. Called off the UI thread.
- The docs say that hosting sends one packet to a public STUN server to learn the address.

## Section 2: the verdict on the address

`Reach.AddressVerdict(string upnpExternal, string upnpWhy, IPAddress stun, int port, out string shareAddress, out bool problem)`, pure:

| Router (UPnP) | Internet (STUN) | Line in the share box | Friends type |
|---|---|---|---|
| public A | A | `Internet address A, confirmed from outside.` | A |
| private or carrier (100.64/10) B | A | `Your router sits behind another router or the provider's shared address (router B, internet A). UPnP opened only the near router: forward port N (TCP and UDP) on the outer one to this PC's router, or use Tailscale.` problem | A |
| public B | A, A != B | `The router says B but the internet sees A: there is another router or the provider's shared address in between. ...` (same advice) problem | A |
| failed (`why`) | A | `Internet address A (port N not opened: ` + why + `).` | A, marked "after forwarding" |
| any | none | `Could not learn the public address (no answer from the STUN servers).` plus today's lines | as today |
| failed | none | today's text | as today |

`ShareLines` already puts an `Internet: <addr>:<port>` line first when given an external address; it now gets the STUN address
whenever there is one (that is what friends reach after any forwarding), and the UPnP address only when STUN is silent.

## Section 3: the port self-test through the router

- `Reach.PortTest(IPAddress addr, int port, int timeoutMs)`: TCP connect to the public address and port, send `HELLO`, wait for one line
  (any line, even an ERROR: it proves the server answered). Returns reachable / no answer / connect error. Measured from Jake's PC:
  the router loops the connection back (NAT hairpin) and the server answers in 2 ms.
- Runs on a thread after the UPnP mapping finishes and the firewall verdict is good; re-runs whenever the verdict turns good (the
  firewall timer already fires every 3 s while something is wrong). Once positive it does not run again for that server.
- Lines: `Port test: friends can reach A:N (tested through your router).` or, not marked as a problem since many routers cannot loop
  back, `Port test: no answer at A:N from inside. Some routers cannot test themselves, so ask a friend to try; if they also get
  nothing, forward port N (TCP and UDP) on your router.`
- Only TCP (the lobby). The match's UDP side is ENet, whose handshake the launcher cannot fake cheaply; the UPnP mapping and the
  firewall rule are checked for UDP already.

## Section 4: the friend's side

`LobbyClient`'s connection timeout message adds: `ask the host what the Port test line in their launcher says`.

## Section 5: tests and docs

- `LobbyTest.cs`: `stun_parse` (XOR form, plain form, wrong cookie, wrong transaction id, no address attribute, IPv6 skipped,
  truncated attribute); `address_verdict` (the six rows above); `stun_live` (a public address comes back); `port_test` against the
  test's own server on 127.0.0.1 (reachable) and a closed port (no answer).
- The Win32-driven UI test (scratchpad) expects the confirmed-address line and the positive port test on Jake's PC.
- Docs: `docs/en/ONLINE.md` host step and troubleshooting, `tools/friends/README.txt` host step and Problems; the connection report
  leaves `TODO.md` and `docs/en/TODO.md`.
