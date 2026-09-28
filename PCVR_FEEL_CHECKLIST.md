# The feel checklist

What the hand-driven weapons get right **today**, written down before step 2 moves
any of it.

This is not a wish list. Every item below is a rule the current code documents as
hard-won, with the line it lives on, and most of them exist because something
simpler was tried first and felt wrong in a headset. The rewrite is allowed to
change how any of it is implemented. It is not allowed to lose the behaviour
without somebody deciding to.

**How to use it.** Check each item in the headset before the old path is deleted,
not after. An item that cannot be checked because the mechanism moved is an item
that needs a new test written for it, not one to tick on the strength of the code
looking right.

Source references are `vr_openxr.c` unless stated. Tuning constants are named
because the number is part of the behaviour: a rewrite that keeps the rule and
loses the constant has kept half of it.

---

## Working the action

- [ ] **The whole stroke sound plays as the motion begins, once per stroke.**
      Not on completion — that is the mod's timing, and it reads as lag because
      the hand has already done the work. Splitting it in two only moved half the
      problem. `:8368`, `vr_action_sound`
- [ ] **Letting go at the back of a stroke still counts.** Releasing used to throw
      the whole stroke away, so a pull that went all the way back and then opened
      the hand — which is how most people actually work a pump — registered as
      nothing. Being drawn back is the part that matters. `:8287`
- [ ] **A slide is a shorter stroke than a fore-end.** One distance served both and
      was tuned on the shotgun, so the pistol inherited a number chosen for a
      different mechanism. A fore-end is about a hand-span; a slide about a
      thumb-length. `:8325`, `vr_pump_travel`
- [ ] **The stroke reference follows the furthest-forward point**, rather than being
      captured once when the hand enters arming range. That range is wide enough to
      latch while the hand is still reaching, after which travel read as far as −32
      units and only an enormous stroke ever crossed back. `:8338`
- [ ] **A hand leaving the gun is not a stroke.** `:8212`
- [ ] **A hand carrying a round is not working the action.** `:8150`
- [ ] **After a round goes in, the hand must open and close again — or leave the gun —
      before it can rack.** `act_rearm`, `act_settle`
- [ ] **A braced off hand on the fore-end does not arm a stroke; a deliberate pull
      does.** `vr_pump_reach`
- [ ] **Only a self-loader cycles its own action on firing.** A pump does not cycle
      itself and a cylinder is not an action at all — kicking part 0 every shot
      would swing a revolver open on every trigger pull. `:6204`, `vr_part_kick`
- [ ] **A stroke abandoned part-way gives up cleanly** rather than latching.
      `vr_pump_giveup`

## Slides and catches

- [ ] **An empty self-loader's slide stays back.** `:6371`
- [ ] **A locked-back slide rests open** and is not dragged shut by the animation.
      `:7957`
- [ ] **An open slide moves only in a hand that is holding it.** A slide nobody is
      holding never closes on its own. `:7979`
- [ ] **A short tug past the stop and release sends it home.** `:6047`,
      `part_off_catch`
- [ ] **A part is only ours while we are actually moving it** — otherwise it is left
      to its own animation, so a cylinder still turns when the weapon fires. `:5955`

## Magazines

- [ ] **A magazine pulled off the end of its travel is out of the gun.** `:6128`
- [ ] **A removed magazine shrinks away over about a fifth of a second and comes
      back seated the same way** — taken out, not switched off. `:6393`,
      `vr_mag_fade` (0.22)
- [ ] **A dropped part-used magazine keeps one in the chamber and loses the rest.**
      `hlsdk: vr_handload.cpp`
- [ ] **A weapon with no magazine model has no magazine to drop.** A revolver has
      nothing to drop and a crossbow keeps its bolts in a rack; the keep-one rule
      would otherwise throw rounds away with nothing visible to explain it.
      `hlsdk: vr_handload.cpp`
- [ ] **A shell for a tube, a magazine for everything else**, and a weapon that
      carries something else says so. `:3559`, `:3568`, `vr_reload_model`,
      `vr_reload_model_mag`
- [ ] **The carried round is hand-sized, not world-sized.** `:3652`,
      `vr_reload_model_scale` (0.0875)

## Revolver

- [ ] **The same reload control that drops a magazine swings a cylinder.** `:8518`
- [ ] **An open cylinder sits where its catch holds it**, not where the animation
      would put it. `:6236`
- [ ] **Muzzle up tips the cases out.** `:6300`, `vr_cylinder_dump` (50)
- [ ] **A flick of the wrist shuts it.** `:6246`, `vr_cylinder_flick` (500)
- [ ] **But not while a round is on its way in.** `:6252`
- [ ] **A closed cylinder has nowhere to put a round.** `:8727`

## Firing and refusing to fire

- [ ] **An open gun does not fire**, and makes no dry click doing it. `:8610`
- [ ] **A state that refuses the trigger does not survive a weapon change.** Open the
      revolver, switch to the crowbar, and the crowbar must still swing. `:8065`
- [ ] **A baseline has to be real before anything is measured against it** — and the
      first clip count ever seen may itself be a shot. `:8097`, `:8113`
- [ ] **A round into an empty gun is not a round in the chamber.** `:8132`

## Loading by hand

- [ ] **The loading port is by the trigger, not the muzzle.** `:8710`,
      `vr_reload_port`, `vr_reload_port_fwd`
- [ ] **Not while that hand is full.** `:2047`
- [ ] **Holding reload for a second always performs an ordinary reload, on any
      weapon.** `vr_reload_hold` (1.0)
- [ ] **Hand loading is per player.** A desktop player on the same server keeps
      vanilla weapons. `vr_handload`, userinfo

## Hands and holsters

- [ ] **Reaching is not asking.** Proximity alone used to fire the holsters, so
      reaching up to throw a grenade swapped to the crowbar mid-throw. Closing the
      hand is the difference between passing a place and choosing it. `:8900`
- [ ] **Two-handed stabilisation holds the weapon steadier without locking it.**
      `:240`, `vr_twohand_radius`, `vr_twohand_barrel`, `vr_twohand_smooth`
- [ ] **The throw arc shows the throw the player is actually winding up.** `:6583`,
      `vr_throw_scale`
- [ ] **Waving the gun around with a hand on a part moves nothing** — parts are
      driven in the weapon's own frame, not the world's. `VR_PartHandLocal`

## Things that must keep being true

- [ ] **A weapon with no card still works**, on the ordinary reload button, and says
      so once rather than failing silently.
- [ ] **The model brings its own shell and the player is already holding one** — the
      reload animation's bolted-on shell stays collapsed. `gl_studio.c`,
      `r_vr_hide_bone`
- [ ] **Desktop players see every part animate normally.** Fixed in `1005ecd6`; it
      is a regression test now, not a bug.
- [ ] **A mod's own action sound is suppressed when we play ours**, so the pump is
      not heard twice. `vr_mute_mod_action`

---

## What is not on this list

Behaviour nobody has confirmed in a headset is not a feel rule and is not
protected by this document. Two known cases:

- The report of a **gun refusing to fire after an interrupted reload** has never
  been reproduced. The "lower-numbered weapon slot" theory for it was wrong — the
  reset fires on any weapon change — so there is no test to write until somebody
  sees it again.
- **Pieces attached to a driven part collapsing** when Half-Life's own model
  renderer draws them would hit the crossbow's bolt rack and the revolver's
  speedloader. Still unverified.
