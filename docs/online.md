# Online play – findings and options

Status: research (2026-10-28). Decision (user, 2026-10-28): **option B**, in Phase 6 (the last phase) of the
roadmap. Nothing implemented yet. Our HLE stubs `nn_ac`, `nsysnet` and `nn_act` to
"offline" (ROADMAP Phase 2), and the game plays fine without a network.

## What the game uses

Facts from the imports and from strings in the v32 ELF:

- **Nintendo NEX, statically linked.** There's no `nn_nex` import because the NEX client library is part of the
  game. Its strings include:
  - login and service: `nn::nex::NgsFacade`, `NetZ`, `PRUDPUserProtocol`;
  - matchmaking: `MatchmakeExtensionClient` (`AutoMatchmake`, `BrowseMatchmakeSession`, `JoinMatchmakeSession`,
    `CreateMatchmakeSession`, `OpenParticipation`), `MatchMakingProtocol(Ext)`;
  - peer-to-peer: `NATTraversalClient`;
  - services: `DataStoreProtocol`, `MessagingProtocol` / `MessageDeliveryProtocol`, `AccountManagementProtocol`,
    `HealthProtocol`, `MonitoringProtocol`;
  - ban codes: `BANNED_ACCOUNT_IN_NEX_SERVICE` etc.
- **System libraries it calls for this:**
  - `nn_act` `AcquireNexServiceToken`: NEX server address and token from the Nintendo account server;
  - `nsysnet`: BSD sockets (29 imports), which the NEX traffic (UDP) goes through;
  - `nn_fp`: friends and presence, `IsJoinable` etc. (32 imports, all `todo`);
  - `nn_ac`: network connection;
  - `nlibcurl`: HTTP, used for the DLC index on `goshawk.capcom.co.jp`, `login.cgi`, and news.
- **MT Framework's network layer has several backends behind one session interface:**
  - `MtNet::Nex::Session`: online through NEX;
  - `MtNet::Uds::Session`: local wireless, for Wii U ↔ 3DS play (`nn_uds`, 28 imports);
  - `MtNet::Snow::Session`: not identified yet;
  - `MtNetRanking`.

  The game logic sees an `nNetwork::Session` / `SessionListener` above them.
- **Service status:** Nintendo shut down the NEX servers for Wii U / 3DS in April 2024, so the original service is
  gone. The game's NEX code is intact. Whether the community server project Pretendo supports MH3U is not known (a
  search on 2026-10-28 found nothing either way).

## Options

**A. Keep the game's NEX client and give it a server.**
- How: real sockets in `nsysnet`, and `nn_act` / `nn_ac` / `nn_fp` HLE that hand the game a configurable NEX server
  instead of Nintendo's.
- Server: a NEX server for MH3U's protocols. That could be self-hosted, built on an existing NEX server
  reimplementation such as Pretendo's Go libraries (AGPL: run as a separate program, a licence question for you), or
  Pretendo itself if it serves MH3U.
- Gains: everything works as the game designed it, including Gathering Hall lobbies, quest join, friends, chat,
  NAT traversal and peer-to-peer play.
- Cost: a NEX server for this game (authentication, secure server, matchmaking extension, NAT traversal, messaging,
  DataStore), plus the game's NEX access key and server settings.

**B. Replace the NEX backend with Cha-Cha's own.**
- How: a new backend behind the game's session interface (`MtNet::Session` / `nNetwork::Session`), with matchmaking
  through a small Cha-Cha lobby server (or a platform such as Steam) and data through direct UDP or a relay.
- Gains: PC-native, no fake Nintendo accounts, and a much smaller server.
- Cost: reverse engineering the session interface and how the Gathering Hall drives it (create/join/search, member
  list, messages). Friend-list features would be reworked.

**C. LAN only, through the local-wireless backend (`nn_uds` over UDP broadcast).**
- Gains: smallest server-free path.
- Cost: on Wii U the local-wireless path exists to host 3DS players, and it's not known whether one Wii U can join
  another. Probably a dead end without changes to the game.

## Recommendation

Start with **A's first milestone**, because it reuses the game's full online logic and settles the unknowns
cheaply:

1. Find the game's NEX settings: game server id, access key, PRUDP version, and the protocols it calls.
2. Make `nn_act` / `nn_ac` report "online" with a configurable server.
3. Implement real `nsysnet` sockets.
4. Log the game's first NEX packets against a local listener.

That shows exactly what a server has to answer. Then choose between running a NEX server (A) and swapping the
backend (B) with real data.

Decisions needed from you:
- A or B as the target.
- If A, whether an AGPL server built on existing NEX code is acceptable as a separate program.
