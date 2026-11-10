/* Addresses for Ocarina of Time (Switch 2), program 0400486024DEC000, update v131072,
 * main build E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.
 *
 * Every value is an offset into the main module (main+0x...), the same convention as
 * docs/FINDINGS.md. The module finds main's base at run time from the nnMain export.
 * "*" in a comment = one pointer dereference of main+OFF, "**" = two.
 */
#ifndef Z64HD_V131072_H
#define Z64HD_V131072_H

#define Z64HD_BUILD_ID          "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348"
#define Z64HD_PROGRAM_ID        "0400486024DEC000"

/* module layout ---------------------------------------------------------------------- */
#define Z64HD_NNMAIN            0x1202CD0   /* exported by main; base = &nnMain - this */
#define Z64HD_CAVE_START        0x3C46400   /* end of .text .. rodata: mapped RX, zero-filled */
#define Z64HD_CAVE_END          0x3C47000   /* (Lichii's own fixes use 0x3c46200-0x3c46278) */
/* 8-byte pointer slot the IPS stub loads the frame hook from. It sits past the bss end
 * that MOD0 declares (0x4e9a4c8) but inside the bss the NSO header maps (to 0x4ea0000);
 * zero in two live dumps. VERIFY with a write watchpoint before relying on it (BUILDING.md). */
#define Z64HD_HOOK_SLOT         0x4E9A4D0
#define Z64HD_ABYSS_HOOK_SLOT   0x4E9A4D8
#define Z64HD_SAND_HOOK_SLOT    0x4E9A4E0
#define Z64HD_VOID_STATE_HOOK_SLOT 0x4E9A4E8
/* Abyss classifier's sole nonzero state commit, before camera/event dispatch. */
#define Z64HD_VOID_STATE_SITE   0x82C690
#define Z64HD_VOID_STATE_RETURN 0x82C614
#define Z64HD_ABYSS_SITE        0x2654B20 /* message sender entry; original PACIBSP */
#define Z64HD_ABYSS_SEND        0x2654B20
#define Z64HD_SAND_SITE         0x31E6A54
#define Z64HD_SAND_SEND         0x373A20
#define Z64HD_VT_ABYSS_NODE     0x4B90830
#define Z64HD_VT_ABYSS_CONDITION_NODE 0x4B909B0
#define Z64HD_VT_SAND_ABYSS_NODE 0x4B91E08
#define Z64HD_VT_AREA_BINDER    0x4B25A60
#define Z64HD_VT_AREA_LOGIC_NODE 0x4C5B298
#define Z64HD_VT_SENSOR_BODY    0x4C37D20
#define Z64HD_SHAPE_CACHED_BOUNDS 0x38614B0

/* frame hook (FINDINGS §3) -------------------------------------------------------------- */
/* Every frame main+0x3b500a4 locks GameData, applies its command queue (main+0xb99980)
 * and tail-calls the unlock at this site. The IPS sends that tail call to the cave stub. */
#define Z64HD_FRAME_SITE        0x3B500DC
#define Z64HD_FRAME_SITE_ORIG   0x1403C8E5u /* "b main+0x3c42470" (original instruction) */
#define Z64HD_GD_UNLOCK         0x3C42470   /* (lock) */

/* GameData (FINDINGS §1, §4) ------------------------------------------------------------ */
#define Z64HD_GD_MGR_PTR        0x4C7E180   /* ** = GameData manager */
#define Z64HD_GD_GETINT         0x163D20    /* bool (mgr, s32 *out, u32 hash) */
#define Z64HD_GD_SETINT         0x163B70    /* void (mgr, s32 value, u32 hash): queued, applied at the flush */
#define Z64HD_GD_INT_TYPE       0xC8        /* manager+: the Int type entry (see gamedata.h) */
#define Z64HD_GD_TYPES_SCAN     0x800       /* other type entries are found by vtable within manager+0..this */
#define Z64HD_VT_INT_FIELD      0x4C20718   /* vtable of each Int field object (not the type entry) */
#define Z64HD_VT_BOOL_TYPE      0x4C1F140
#define Z64HD_VT_FLOAT_TYPE     0x4C1F3C0
#define Z64HD_VT_ENUM_TYPE      0x4C1F500
/* String64 fields: found from the Sequence_CurrentBanc entry (index 0x1F) in their hash
 * table, exactly as game.py does (STR_ANCHOR / STR_FIRST_VALUE / STR_STRIDE). */
#define Z64HD_STR_ANCHOR_HASH   0x1D6189DAu /* Sequence_CurrentBanc */
#define Z64HD_STR_ANCHOR_IDX    0x1F
#define Z64HD_STR_FIRST_VALUE   0x207C
#define Z64HD_STR_STRIDE        0x108

/* pouch (inventory) code (FINDINGS §3, §4) ---------------------------------------------- */
#define Z64HD_POUCH_MGR_PTR     0x4C7E0D0   /* ** = pouch manager */
#define Z64HD_POUCH_ADD         0x110D570   /* see pouch.c */
#define Z64HD_POUCH_NOTIFY      0x110D0B0
#define Z64HD_POUCH_NOTIFY_PTR  0x4E6B718   /* * = notify target */
#define Z64HD_POUCH_REMOVE      0x33A4FA0   /* (pouch mgr, &name, count, 0, 0, 0) */

/* Link's gear (FINDINGS §5.1, §5.4) ----------------------------------------------------- */
#define Z64HD_EQUIP_SLOT_FN     0x2DF6C80   /* (equipment component, slot) */
#define Z64HD_EQUIP_ALL_FN      0xF201B0    /* (owner, 0): re-apply all, takes off empty slots */
#define Z64HD_VT_EQUIP          0x4B49698   /* equipment component; +0x18 = owning actor */
#define Z64HD_VT_EQUIP_OWNER    0x4BDFE68   /* holds the component at +0x470 */

/* UI (FINDINGS §4) ---------------------------------------------------------------------- */
#define Z64HD_UI_MGR_PTR        0x4C7E098   /* ** = UI manager; +8 count, +0x10 screens */
#define Z64HD_VT_DPAD           0x4BE1068   /* D-pad widget, at HUD screen +0x330 */
#define Z64HD_VT_HUD            0x4BEB710   /* PlayerStatus screen; method 98 = main+0xa77ff0 */
#define Z64HD_VT_MAP_SCREEN     0x4BE8300   /* pause map screen; method 98 = main+0x33e12c0 */
#define Z64HD_UI_REFRESH        (0x310 / 8) /* vtable slot 98: "refresh contents" */

/* events (warps) -------------------------------------------------------------------------- */
/* How an exit starts a map change: FieldAndDungeonEntranceEventRequestActor requests event
 * "Dm_SY_BancChangeGo" with string params NextBanc / NextStartPos / NextStartPosAir /
 * BootEventName and bool IsCheckAir (built by QueryBancChangeEventRequestArg, main+0x29bf380);
 * the flow's GameSystemActor.EventReserveBancChange then does the rest. Requests are made like
 * the SeqOneShotRequestEvent node does (main+0x32db1a0). */
#define Z64HD_EVENT_MGR_PTR     0x4C7E090   /* ** = event manager */
#define Z64HD_EVENT_REQ_CTOR    0xD79F10    /* (request) */
#define Z64HD_EVENT_REQ_SUBMIT  0xD7A820    /* bool (event manager, request) */
#define Z64HD_EVENT_REQ_SUB_DTOR 0x20B6F0   /* destructor of the two sub-objects below */
#define Z64HD_EVENT_REQ_SUB_VT  0x4974B10   /* (+0x10) their base vtable, set before destroying */
#define Z64HD_EVENT_REQ_SIZE    0x1B20
#define Z64HD_EVENT_REQ_PARAMS  0x08        /* param entry array (32 inline entries at +0x18) */
#define Z64HD_EVENT_REQ_NPARAMS 0x10        /* u32 count; capacity (u32) at +0 */
#define Z64HD_EVENT_REQ_SUB_A   0x1950
#define Z64HD_EVENT_REQ_SUB_B   0x1968
#define Z64HD_EVENT_REQ_NAME    0x1A90      /* char *event name; capacity (u32) at +0x1A98 */
#define Z64HD_EVENT_PARAM_SIZE  0xC8        /* entry: u32 type, char *name (+8, cap u32 +0x10), value +0x98 */
#define Z64HD_EVENT_PARAM_STRING 1          /* value: char * */
#define Z64HD_EVENT_PARAM_BOOL   4          /* value: u8 */

/* Link, camera, frame (game.py) --------------------------------------------------------- */
#define Z64HD_VT_LINKOBJ        0x4B25E18
#define Z64HD_VT_ENTITY_BODY    0x4C378C0
#define Z64HD_VT_MESH_WRAPPER   0x4C31DE8
#define Z64HD_VT_SHAPE_RESOURCE 0x4C31EE0
#define Z64HD_VT_SHAPE_DESCRIPTOR 0x4C31F00
#define Z64HD_ACTOR_MGR_PTR     0x4C7DC40 /* ** = process manager; actor list +0x68 */
#define Z64HD_VT_POSITION_CALCULATOR 0x4B47058
#define Z64HD_VT_PARAM          0x4B63438
#define Z64HD_VT_ENEMY_PARAM    0x4B4C400 /* ActorLife current/max s32 parameter */
#define Z64HD_SCENE_MGR_PTR     0x4E97648
#define Z64HD_VT_STATIC_MGR     0x4BA1FE8
#define Z64HD_VT_STATIC_BODY    0x4C324F0
#define Z64HD_VT_STATIC_RESOURCE 0x4C32600
#define Z64HD_VT_HK_COMPOUND    0x4891430
#define Z64HD_VT_HK_MESH        0x4893068
#define Z64HD_VT_HK_BOX         0x48936E8
#define Z64HD_VT_HK_CYLINDER    0x4893780
#define Z64HD_VT_HK_CONVEX      0x4893AE8
#define Z64HD_DISP_PTR          0x4C7E740   /* ** + 0x29C = Link's drawn position (3 floats) */
#define Z64HD_CAMERA_PTR        0x4E98068   /* [*+0x68] = camera: +0x38 pos, +0x44 look-at */
#define Z64HD_FRAME_COUNTER     0x4E84BA0   /* u32, +1 per update; runs in the pause menus too */

/* Character controller: verified against the game scripts' native nodes and live
 * object vtables. See FINDINGS, "Native collision bypass". */
#define Z64HD_VT_CCT_PROPERTY   0x4C34F10
#define Z64HD_VT_CCT_BODY       0x4C34658
#define Z64HD_CCT_FORCE_NO_HIT  0x158D40    /* void (body, bool): queued, preserves filter state */
#define Z64HD_CCT_SET_POSITION  0x6B2530    /* void (property, const vec3f_t *): updates attached bodies */
#define Z64HD_CCT_SET_VELOCITY  0x12DBE0   /* void (body, const vec3f_t *): queues linear velocity */

/* (main+a) -> +b -> Link's actor, tried in order */
#define Z64HD_LINKOBJ_PTRS \
  { { 0x4E371D0, 0x480 }, { 0x4E94858, 0x2E0 }, { 0x4E935B8, 0xEF8 }, \
    { 0x4E94860, 0x328 }, { 0x4E94368, 0xD70 } }

#endif
