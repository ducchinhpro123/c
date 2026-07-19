---
status: accepted
---

# Broadcast File Offers with closed Recipient Sets

Relay treats a dropped file as a File Offer to every Participant currently in the Relay Workspace. The Relay Server closes the Recipient Set after every response or a 60-second Offer Window, then the Sender streams once and each Recipient receives an independent Delivery; this preserves explicit consent and isolates failures without requiring replay buffering or allowing a late Recipient to receive a partial file.
