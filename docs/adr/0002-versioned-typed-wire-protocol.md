---
status: accepted
---

# Replace the v1 wire format with a typed v2 protocol

Relay client and server move together to a versioned v2 wire protocol with one shared bounded codec and typed, length-prefixed payloads. Relay deliberately does not carry a dual-stack v1 implementation: the clean cutover keeps framing and validation local to one module, while the accepted cost is that older clients cannot connect to a v2 Relay Server.
