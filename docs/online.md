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

## Local play: what the binary allows (2026-11-07)

Mapped from the recompiled code (factories and constructors found by their vtable addresses; `config/vtables.csv`).

- **Three layers.** Game: `sLobbyNetwork` (the hub; has its own `MH3GCafeTransport`), `sMH3Network`, `cMH3Session`
  (an `nNetwork::Session`), `sMH3NetworkManager`. Engine, backend-agnostic: `nNetwork::Session / SessionDriver /
  Transport / Protocol{Standard,Broadcast,Minimum} / Connect / Match / Member / nSharedMemory` with RPCs
  (`RpcNetSystem_TryConnect / Entry / Match / Leave / HealthCheck ...`). Backends: one abstract interface each -
  `MtNetContext` (vtable 0x100F1268, 40 slots), `MtNetSession` (0x100F29D8, 42), `MtNetP2p` (0x100F2260, 20) -
  implemented by `MtNet::Nex`, `MtNet::Uds` and `MtNet::Snow` with exactly those slot counts.
- **Backend factories.** Session: `0x02E4D9AC(?, context r4, kind r5)` - kind **3 -> `MtNet::Uds::Session`**
  (ctor 0x02E6BB9C, 0x24540 bytes), **4 -> `MtNet::Nex::Session`** (ctor 0x02E728B4, 0x2E580), **0x10 / 0x11 ->
  `MtNet::Snow::Session`** (ctor 0x02E7A544, 0x2E5D0, gets the kind), anything else null. Context: `0x02E4D844`
  (ctors Uds 0x02E66DE4, Nex 0x02E6D8C8, Snow 0x02E75D34), called from `nNetwork::Context` (0x02CD0D78). The
  session kind comes from `nNetwork::SessionDriver` +0x60 (0x029876CC -> 0x02985090 -> factory), set by the game when
  it opens a session. `MtNetMediator` (ctor 0x02E4F5AC) routes packets between them.
- **Snow** is Capcom's lobby middleware on top of NEX (`CNEXLobby::PhaseLoginLobby... / GetRooms / GetParticipants /
  ChatLobby`, `CNEXSystem::IphaseUpdateBeacon`): the Gathering Hall's lobby and room lists, and it bridges 3DS players
  on local wireless into an online session (`SnowNexToUdsRecvPacketCallback`, `SnowUdsToNexRecvPacketCallback`).
- **Local wireless can only host.** The game's `nn::uds::Cafe` imports are CreateNetwork, CreateBridgeNetwork,
  AddOutsideClient, AllowToConnect / DisallowToConnect, EjectClient, beacons, SendTo / ReceiveFrom (+ bridged) - no
  Scan and no ConnectNetwork. A Wii U hosts 3DS players; it never joins. Two copies of the Wii U game cannot meet over
  this path (option C above is ruled out), and the 3DS side of the protocol is the 3DS game's code, which we do not have.
- **So local play between Cha-Cha instances = the online path with a local backend.** On the console, Wii U <-> Wii U
  play only ever went through NEX / Snow. Plan (option B, LAN first): a Cha-Cha backend that the two factories return
  for kinds 4 and 0x10 / 0x11 (guest objects whose vtables point at host code), implementing the 40 + 42 + 20 virtual
  functions over UDP: LAN discovery by broadcast in place of NEX matchmaking and Snow's lobby / room lists, direct
  UDP between the instances for `MtNetP2p`. The same backend covers two instances on one PC (couch co-op: each its own
  window, controller and data folder, over loopback). nn_ac / nn_act / nn_fp must report "online, signed in" for the
  Gathering Hall's online door to open.
- **Next steps.** (1) Runtime probe on the factories and +0x60 with a save in Port Tanzia: which kind the Gathering
  Hall asks for, and what it does when the stubs say offline. (2) Name the 102 virtual slots from the Nex / Snow
  implementations (what each takes and returns, which listener callbacks they fire). (3) A loopback backend between
  two instances on this PC, then LAN broadcast discovery.
