# Relay

Relay is a trusted-LAN workspace where Participants exchange Chat Messages and File Offers through a Relay Server.

## Language

**Relay Workspace**:
The live collection of Participants connected to one Relay Server. It has no durable membership.
_Avoid_: Room, channel, account

**Relay Server**:
The authority for Participant identities, Chat Message attribution, File Offers, Recipient Sets, and File Transfer routing within one Relay Workspace.
_Avoid_: Backend, central peer

**Participant**:
A connection-scoped presence in a Relay Workspace under a server-assigned identity. Reconnecting creates a new Participant even when the same Display Name is reused.
_Avoid_: Client, peer, user

**Display Name**:
The human-readable label chosen for a Participant. It is presentation, not durable identity.
_Avoid_: Username, account name

**Chat Message**:
Text contributed by a Participant and relayed with that Participant's identity authored by the Relay Server.
_Avoid_: Text packet, message string

**Sender**:
The Participant who creates a File Offer and supplies its file bytes. A File Offer ceases to exist if its Sender disconnects.
_Avoid_: Uploader, sending client

**File Offer**:
An invitation from one Participant to every other Participant connected when the offer is created. It closes after every invited Participant responds or when it expires.
_Avoid_: Transfer request, incoming file

**Declined File Offer**:
A File Offer that closes with an empty Recipient Set. It ends without creating a File Transfer and is not a transfer failure.
_Avoid_: Failed transfer, rejected file

**File Transfer**:
The delivery phase of a closed File Offer from its Sender to its closed Recipient Set.
_Avoid_: Upload, download, file stream

**Delivery**:
The part of a File Transfer directed to one Recipient. Each Delivery succeeds or fails independently without deciding the outcome of other Deliveries.
_Avoid_: Sub-transfer, download

**Recipient**:
A Participant who accepts a File Offer and is therefore eligible to receive its file bytes.
_Avoid_: Receiver, accepting client

**Recipient Set**:
The collection of Recipients captured when a File Offer closes. No Participant can join after File Transfer begins, but a Recipient can leave through disconnect or local failure.
_Avoid_: Accepted clients, receiver list

**Offer Window**:
The server-owned 60-second period during which invited Participants may accept or reject a File Offer. It ends early when every invited Participant responds.
_Avoid_: Acceptance timeout, response timer

**Received File**:
A local file whose File Transfer completed successfully for that Recipient. Partial data never constitutes a Received File.
_Avoid_: Download, partial file
