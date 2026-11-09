# Online play

**Status (2026-11-09, released in 0.3.0 as very experimental): Host and Join played by a person with two copies on one PC; quests only in automated headless tests.**
Talk to Neko: his Destination list has an Online section with Host a Hunt, Join a Hunt and Leave Online. Settings -> Gameplay -> Online has the status, Leave, and "Play over the network" (off by default: copies meet over UDP on 127.0.0.1).
`scripts/online-two-copies.sh` starts two windows. Sections below are in the order they were found; older ones can be out of date, "Two copies" and later hold.

| | State |
|---|---|
| Host / Join from town, Player List, both hunters visible, positions match | Seen working |
| Guest leaves and rejoins; host leaves and the session ends for the guest | Seen working |
| Guest follows the host's quest; member barrier clears | Seen working, with the depart press imitated by the probe |
| Large monster: one copy controls it, the other follows its route, control moves between members | Seen working |
| Damage: same HP on both copies, whether the attacker controls the monster or not | Seen working (HP 1433 -> 1379, 1433 -> 1382) |
| Back in the hall with session and Player List after a quest ended | Seen once per copy; why that quest ended was not checked |
| Joining during the Port Tanzia flyover: 30 FPS lock and an empty counter on that copy | Fixed, seen working in headless host + joiner runs; not yet retried by a person |
| The real quest counter and its depart button | **Not tried: needs a person** |
| A second quest in one session | **Not tried** (logic for it is written) |
| Carving, rewards screens, finishing a hunt by killing the monster, chat | **Not tried** |
| Two PCs | **Not tried.** Network mode has only run with both copies on one PC |
| Online on Windows | **Not tried.** The Windows build compiles the same code; only Linux has run it |
| Three or four players, host leaving during a quest | **Not tried** |
| Neko's menu opens and its rows Host, Join, Leave work | Seen working, with the menu opened and the cursor placed by the probe; talking to him and the d-pad not tried by script |
| The game's own Network Mode flow past "Start Network Mode?" | **Replaced, not implemented**: Yes leads to the lobby browser, which needs Snow's lobby operations (Internet states 22-30) |

Small monsters are local to each copy by the game's own data (state +0x9C bit 8 unset): their positions differ, as designed.
Testing trap: copies read any attached controller, in or out of focus. Headless tests need `SDL_GAMECONTROLLER_IGNORE_DEVICES`.

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

## The 102 backend slots, mapped (2026-11-08)

Static reading of the recompiled code only: nothing here has been run. Slot = vtable entry index (entry 0 is the
typeinfo, so `symbols.txt`'s `vfN` is slot N+1); a virtual call loads offset `slot*8+4`. `?` = inferred, not proven.

**The real size.** 102 entries, but entries 0-10 are typeinfo / `MtNetObject` plumbing and the last ones of Session
(37-41) and P2p (16-19) and Context (36-39) are inherited code that works for any backend. Pure virtuals (all pointing
at 0x03122C18 in the base): Context 11-35, Session 11-36, P2p 11-15 = **56**. Going by the Nex versions, about half of
those are one-liners or no-ops: **about 21 hold real logic, 8 copy structs out, 27 are trivial.**

**The base class does the hard part.** `MtNetSession` already has the request queue, the rule for which requests may
overlap (slot 38, 0x02E60C74) and a helper for every listener callback (0x02E61238-0x02E630C0, member +0xB8). A new
backend reuses these guest functions, so the 44 listener slots (26 + 10 + 8) need no new code.

**Session requests.** The engine queues a code (`02E602A0`, controller at +0x4C), and slot 39 (0x02E61048) runs the
handler for it, called again until the request finishes:

| Code | Queued by | Args | Slot | Uds / Nex / Snow size | Meaning |
|---|---|---|---|---|---|
| 0x301 | 02E61E2C | 4 | 29 | 488 / 526 / 311 | create session (`CREATE OPTION`; wrapper copies the option in via slot 14 first) |
| 0x302 | 02E61E70 | 3 | 30 | **stub** / 328 / 218 | search? (`SEARCH OPTION`; result = count + 100 entries of 0x744 bytes, 02E61C5C) |
| 0x303 | 02E61E94 | 1 | 31 | **stub** / 763 / 584 | join? |
| 0x304 | 02E61EB0 | 0 | 32 | 141 / 138 / 95 | leave? (queued from the teardown path, 02E62260) |
| 0x305 | 02E61EC8 | 1 | 33 | 113 / 174 / 17 | ? |
| 0x306 / 0x307 / 0x308 | 02E61EE4 / 02E61EFC / 02E61F14 | 0 | 34 / 35 / 36 | 9 / 33 / 29 (same in all three) | small: fire listener 24 / 25 / 26 |

Other Session slots: 11 update? (per frame, wrapped by base 02E62260 / 02E6246C), 12 compare two entries?, 13 flag read,
14 set create option (0x90 bytes at +0x20E0), 15 set attribute? (wrapper 02987D00, from 0226A930), 16-21 copy state out
(sizes 0x674, 0x204), 22 flag, 23 no-op in all three, 24 member connected? (fires listeners 5, 7, 14), 25-27 packet /
member handling (Uds 26 parses packets through `MtNetBuffer`; error 0x80050031 in 27), 28 finish-or-fail wrapper.

**Context.** Slots 34 and 35 are the sign-in? and sign-out? state machines (399 and 122 instructions on Nex; Uds polls
`02E5A47C`, fails with 0x80000012). 11 update?, 12, 18 and 25 are small; 26-33 are `return` in all three backends, and
13-17, 19-24 are getters and flags.

**P2p (the data plane, only 5 slots).** 11 send? (packet limit 0x5C8, error 0x80030036), 12 state read?, 13 receive?,
14 close? (handles reset to -1), 15 open state machine? (requests; 0x80030002).

**Who gets which kind.** `sMH3NetworkManager`? (by address range) +0x190 is the mode: mode 1 pairs with kind 3 (Uds), mode 4 with kind 0x10
(Snow), per 0226EA3C. So online is Snow, and Snow / Nex / Uds are the only three to replace or imitate. The three
factories are called from 0x02985090 (session) and 0x02CD0D78 (context); 0x02E4DAB0 is the P2p factory? (same args,
result goes to the Transport).

**What goes around the interface.** Calls from outside into the backend range are nearly all the shared base classes,
storage and time. The exceptions, which a replacement backend must also answer:
- `cCafeTransport` ctor / dtor (02246DA0 / 02246EC8) register a callback with Snow (02E53A64 / 02E53A70). **Unknown
  what it carries.** It sits under `sLobbyNetwork`, so it could be the Gathering Hall's avatars. This is the main risk.
- `sLobbyNetwork` slot 6 (02266E88) calls 02E4DEC8 / 02E4DF0C / 02E4DF50 (Snow state).
- `cNetStorage` (02249874) calls 02E4DC6C.

**Config found on the way.** 02E4D124 fills default NEX settings with `0903b15042641c7908ff88eaa565f861` and
`deed2a88` (NEX access key?), plus 0x481 (meaning unknown; `sNetwork` ctor 02BC4890 passes it too). Only useful for
option A; unverified. The game server id is 0x10104D00, printed by the game (probe below).

**Plan.** Each step proves the next is worth doing:
1. Probe: log the factories, the requests and the mode; report online from `nn_ac` / `nn_act` / `nn_fp`; try to enter
   the Gathering Hall. Settles which kind is requested and which gates exist.
2. Solo backend: entries of Snow's vtables point at host functions (`register_indirect`); the game still constructs
   the Snow objects, so no factory override or code regeneration is needed. Done, see "Solo backend" below.
3. Two instances on one PC: search / join over loopback, then P2p send / receive.
4. Quest together, then LAN broadcast discovery.

## Probe results (2026-11-08)

`src/game/net_probe.cpp`, off unless `MH3U_NET_PROBE` is set (`1` logs, `online` also reports a signed-in console;
`MH3U_NET_PROBE_AREA="frame:dti"` changes area, `MH3U_NET_PROBE_FLAGS="frame:bits"` or `"frame:i:bits"` sets request bits).
Run headless on a copy of a save: area change to `aHarborSquare` (0x1030CAE8) at swap 3500, request at swap 6000. The
requests were set by the probe, not through the game's menus: the real door has not been walked through yet.

- **Two state machines in `sLobbyNetwork`.** Local: 02260834, state +0x38, requests +0x44, opens the manager in mode 1.
  Internet: 02266E88, state +0x8AC, requests +0x8B0, opens it in mode 4. Bit 0x2 starts either. Both go 0 -> 1 -> 11,
  and to 13 on failure, then shut down cleanly with no crash. (0225F0D4 uses mode 2, 022732D0 mode 3: not looked at.)
- **Local (bit 0x2 at +0x44) -> `Uds::Context`.** Fails when `nn_uds` Initialize fails.
- **Internet (bit 0x2 at +0x8B0) -> `Snow::Context`.** Confirms mode 4 means Snow for the context. The session was not seen in these runs: the
  failure comes first. With the solo backend it is `Snow::Session` and `Snow::P2p`.
- **Slots confirmed by these runs.** Context 24 runs once at creation, 11 every frame, 34 polled during sign-in, 35 polled
  during sign-out, then the destructor.
- **Internet sign-in order with `online` answers:** `nn_fp` Initialize, `nn_ac` Initialize / Connect / GetAssignedAddress,
  socket_lib_init, NSSLInit, curl_global_init_mem, `nn_act` GetAccountId, `nn_fp` IsInitialized / SetNotificationHandler,
  OSReport "Success to network initialize.", IsApplicationConnected, GetPrincipalId, HasNfsAccount, then
  AcquireNexServiceToken, which still fails: OSReport `GameID:10104d00 Success:0`. That is the first gate left.
- **Consequence for option B.** Snow's sign-in calls the system libraries itself, so the replacement context answers
  slot 34 with "signed in" and none of this needs faking in the shipped path.

## Solo backend (2026-11-08)

`src/game/net_backend.cpp`, off unless `MH3U_NET_BACKEND` is set (`solo` at the time; see "Two copies" below). It replaced three slots of Snow's vtables and no others:
Context 34 (sign-in), Context 35 (sign-out), Session 29 (create). Create adds this machine as member 0, host, through the
game's own add-member routine 02E784C4 (a kind-4 `MtNetUniqueId` from a 16-bit node number, as Snow's event handler builds
it), then fires the listeners Snow fires. Session +0x77D / +0x77E stay 0, so Snow's event polling (02E7872C) returns at
once and Snow's leave (slot 32) skips its server call. Nothing is sent anywhere.

**Seen in headless runs** (Port Tanzia by area change, requests set by the probe at +0x8B0: 0x2 sign in, 0x4 session stage,
0x8 create, 0x40 leave):
- Internet states 0 -> 1 -> 11 -> 17 (signed in, hub) -> 2 -> 3 (session hub) -> 4 -> 9 -> 10 (in session). Leave: 15 -> 16,
  Session and P2p destroyed, back to 17.
- With a session, area change to `aHarborRoomSquare` (0x1030CA00) shows the game's Player List with the hunter's name, and
  Toggle names / Start chatting. "Start chatting" also shows in Port Tanzia, and is still there after leaving (state 17).
- Request 0x301 -> slot 29 and options through slots 14 / 15 are now confirmed by running (sizes 0x90 and 0x204).

**Not done or not known:**
- The requests are set by the probe. The game's own menu has not been used, so create ran with empty options and capacity 0.
- `sLobbyNetwork` state 4 needs its caller's arguments (02271C90): search and join (bits 0x10, 0x20) are untried.
- `cCafeTransport`: no problem seen alone, but alone it may have nothing to send. Still the main risk.
- P2p: only slots 11 and 14 are called so far; send and receive have not run.

## Two copies (2026-11-08)

Same file, `MH3U_NET_BACKEND=1`. One UDP socket per copy on the first free port of 47800-47807, **bound to 127.0.0.1**: nothing
leaves the machine. Search asks every port in the range; the host answers. Messages: search request / reply, join request /
reply, member added, member left, data. Slots replaced: Context 12, 34, 35; Session 11, 29, 30, 31, 32; P2p 11, 13, 14, 15. Join
does the exchange itself, then hands over to Snow's own phases 4 and up (0x02E7B8B8), which never touch the server.

**Seen in two headless copies on this PC** (same save copied twice, requests set by the probe, area changes by the probe):
- The client's search found the host's session, it joined as member 1 of 2, and both copies reached state 10 and stayed there.
- **Both copies show the game's Player List with two rows.**
- About 2 packets a second each way at roughly 40 bytes, for over two minutes; sent and received counts agree, allowing for
  the packets in flight when each total is printed.
- P2p open: the u16 at arg + 0xE is the member index + 1 (the value stored at info +0xC8), not a node number.

**Not working or not known:**
- **The other hunter is parked, not missing.** Measured with `MH3U_NET_PROBE`'s world log, entering `aHarborRoomGuild` (0x1030C918)
  in state 10: `sPlayer` (*0x10314B80) +0x40 holds 4 `uPlayerTest` units on both copies, one per member slot (pad +0x3C byte). As the
  client walks to -995, -137.5, -272, its unit on the host stays at the default -1600, -137.5, -366. After the first burst only
  size 32 and 40 packets flow, so movement is not being sent. `aHarborSquare` / `aHarborRoomSquare` only build 1 unit.
- **Hunter data does cross:** when the copies loaded different hunters, both Player Lists read "T e s t" and "Silvers".
- **Both hunters are visible when the area is built on stage 1B/1.** The Harbor areas take the stage from `sLobby` (*0x103147A0) +0x5450:
  +1 stage, +2 and +0x15 sub-area (0x0214CE3C fills them from `sGame` +0x58 == 1), passed to the stage setter 0x02190C50 through
  0x0214CEE0. Debug jumps leave sub-area 0. `MH3U_NET_PROBE_SUB=1` writes +2 and +0x15 before the jump to `aHarborRoomGuild`
  (0x1030C918): both copies then read 1B/1, spawn at 210, 6.3, -330 on real ground, and **each screen shows both hunters**. A short
  client step gives 409.3, 6.3, -373.2 on both copies.
- Why the stage matters: the hub position sender 0x022627CC refuses 1B/0 and the receiver 0x02262DA0 needs 1B/1. The hunter record
  handler 0x0226429C only applies the appearance on 1B/1, and parts stay hidden until their load states finish (0x02847ADC,
  static reading). `MH3U_NET_PROBE_HUB=frame:1b:1`, writing the id after the area is built, moves the unit but never draws it, and
  loses collision. Keep it as a diagnostic only.
- Ruled out: `sGame` +0x61, +0x48 (already 1 and 2 before any write); `sAppProcedure` +0xC3C, forcing it changes nothing in the hub
  (it gates 0x02254D3C / 0x02256080, set by quest-area code, so that is the quest channel (?)). 0x0218D9D8 is a table lookup, not a stage load.
- Still unknown: what sets `sGame` +0x58 to 1 on the game's own route into the hall. Many functions write +0x58; the right one is not identified.
- **Quest launch (probe, `MH3U_NET_PROBE_QUEST=frame:number`).** Doing the steps of 0x0214E8A8 on both copies shows the game's "Launching
  Quest..." and the quest map loads: `aQuestArea` (0x1030CEA8) reaches +5=2 and stays there, on the network barrier at `sAppProcedure`
  +0xF84 / +0xF8C (static reading: members send category 0x11 type 0xA through 0x0225C078, the host answers type 0xB through 0x0225C108).
  `sGame` +0x4B is 255 there and the party table `sAppProcedure` +0xC40 + i*0xD0 is empty, so the host check 0x02261D48 cannot pass (?).
- **Quest post (probe, `MH3U_NET_PROBE_POST=frame:number`).** 0x021E355C(sLobby, my pad block, number, 1) is the quest select. It creates the
  party and returns 0, but `sAppProcedure` +0xC40 stays empty: 0x0225B238 builds that table from the party's bitmask (entry 0x9C4 each, +0xB).
- **Two copies in one quest (probe, `MH3U_NET_PROBE_PARTY=frame:host_member:mask`, standing in for the join handshake).** Both copies post quest
  1101, then each sets its party mask to 3, runs 0x0225B238 and sets `sGame` +0x4A to the host's member, then launches. The barrier clears
  (`aQuestArea` +5=3), `sGame` +0x4B is 0 / 1, +0xC3C = 1, and both copies are in the quest (timer, party list with two names). The client's
  hunter reads the same exact position on both copies, about 180 packets a second in all, 80-104 bytes.
  Not done: the real join handshake (how a guest's bit reaches the party; 0x02251380 and the 0x022646F4 mode 2 path are the leads). Monsters, damage,
  and finishing a quest are untested: 1101 is a gathering quest.
- **Monsters.** `sEnemy` +0x34: slots 0, 1 large, 2 on small. state = unit +0x1BE0: +0xEB zone, +0x8B8 HP, +0x9C bit 8 shared (from the monster's data), +0xFF this copy controls
  it, +0x101 the controlling member (0x025A3A1C). Unit +0x40 position. Quests 2201 and 2301 have no large monster; 1407 does. Online, only the host spawns it (0x0216D998 -> 0x020F2E44 -> message 0x11 / 0xF;
  receiver 0x0225A714). With quest 1407 the guest's copy follows the host's route exactly, and HP drops match. Copies' frame counters differ, so compare routes, not frames.
- **Quests together.** The game's party messages (object at sLobby +0x4E20, dispatcher 0x0224E3E8) do not reach a guest: a guest's join (0x021E355C mode 2) returns 2 with empty entries. Not known why.
  So `net_backend.cpp` watches the host's sLobby +0x522C (quest) and +0x54AE (bit 8 depart), sends them over its own wire, and fills each copy's party mask (entry +0xB) from the session. The host's own launch
  state machine 0x0214E2B8 runs by itself after a depart; guests do its steps. The depart recomputes the party, so the mask is filled again then and the table rebuilt (0x0225B238).
- Untested: session slot 33 (request 0x305).

**The real menu route (static reading, nothing run; `?` = inferred).**
- `sLobbyNetwork` setters of +0x8B0: 0x02267D38 bit 0x2, called by the `uIDLobbyRelayLobby` runner 0x02731134; 0x02267D48 bit 0x4,
  from `uIDLobbyRelayList` 0x027251EC; 0x02267DAC bit 0x8, 0x02267DF4 bit 0x10 and 0x02267E50 / 0x02267E64 bit 0x20, all from
  `uIDLobbyRelayList` 0x02725298; 0x02266BB4 bit 0x40. 0x02267E90-0x02267F78 set "lobby op" bits at +0x954, which likely lead to
  states 22-30 (02271EC4 ...), Snow's lobby calls. Those are not replaced yet.
- 0x02265DB4 is an event callback (?), referenced from data at 0x10028D6C, not a request setter.
- `uIDLobbyOnline` (id 0x40) creates the RelayLobby child (id 0x79) in slot 0x026AC5E4; its states 0x1E / 0x1F drive it. Its Local
  states use the +0x44 setters. What opens id 0x40, which NPC or door, is **not found**.
- `nn_fp` GetMyPresence is called directly by the UI at 0x0272A280. `nn_fp` has no HLE, so that call aborts the game: it needs
  implementing before the real menu can work.

## Neko's menu (2026-11-08)

Neko's Destination list is `uIDLobbyOnline` (screen 0x40, layout `GUI\\world\\sho_net1`, created at startup: `sID::IDLobby::vf7` 0x021BA284): this is the game's door to online. Talking to him: `uLobbyNpc` 0x02798F68 ->
`sLobby::startFerry` 0x021CBA58 (mode 0x28) -> driver 0x021CBD24. The screen's states are in 0x026B1DAC (+0x3F4); state 9 is the list (0x026AD984, overridden by `ferry_list` in menu_mouse.cpp). Rows: 0 Moga, 1, 2 Solo, 3, 4 Multi, 5 Network Mode.
Multi is local play with the 3DS game, which cannot work here, so rows 3, 4, 5 are Host, Join, Leave: `ferry_choice` in net_backend.cpp answers a decide press, and closing turns the press into the game's cancel for one call. Rows are painted by what is usable now
(`FUN_026ACBB8`). Text: group 0x67, 0x239 "Multi", 0x23C / 0x23D rows (shared with Solo, told apart by caller), 0x24C Network Mode.
Row 5 in the game's own flow: state 0x10 -> "Start Network Mode?" -> 0x1C, 0x1D, child `uIDLobbyRelayLobby` (0x02731134), which signs in on our backend (Internet state 17). Past Yes it wants the lobby browser, not implemented; the rows above replace it.

## The arrival flyover (2026-11-09)

Reported by a person: the joining copy had frame drops and no NPCs at the counter. Measured with the player's own settings and save: host 6.9 ms a frame (fps_limit 144), joiner exactly 33.3 ms, its lobby NPCs (`sLobbyNpc` 0x10314890 +0x34, spawned by 0x02151080) all at 0,0,0. No pipelines were compiled on either copy, and the saves were identical, so neither explains it.
- Arriving in Port Tanzia, `sEvDemo` (0x103138C8) can queue an event of type 0x10, sub-area 0: the camera flyover of the harbor, about 28 s. It runs in states +0x34 2, 3, 4 and starts with `sMain` (0x103CEA44) +0x76 fixed step on, +0x4C = 30, +0x88 = 2 (0x0294C968, called from 0x0210F7D0). Only the end of its state 4 undoes that.
- `Step::ToHall` changed area during it. The area change resets the state to 0, so the pacing stayed at 30 FPS until the game restarted, and the hall's NPCs were never placed.
- Fix: `Step::ToHall` waits for the flyover (`cutscene_playing`, up to `kCutsceneWaitFrames`). `end_stuck_cutscene` undoes the pacing if one was cut off anyway; forced with the wait at 0 it restores the frame rate but **not** the NPCs, so the wait is what fixes them.
- Seen after the fix: flyover ended by itself, joiner about 8 ms a frame (worst about 10 ms), NPCs at the same positions as the host's, counter populated with speech bubbles.
- Not known: why a copy that takes Neko's row at swap 7800 gets the flyover and one at swap 5300 does not (both rows 3 and 4 get it at 7800, so it is not the row). The host only avoided it through that timing, and uses the same step, but a late host has not been run since the fix.
- `MH3U_NET_PROBE_NPC=1` and `MH3U_NET_PROBE_PACE=1` log the NPC slots and the pacer / `sEvDemo` state.
