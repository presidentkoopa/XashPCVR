Weapon cards, by game directory.

A card is bound to the model it was measured from, by fingerprint, so the
directory a card lives in is not what makes it apply - the fingerprint is.
The directories here are for people, not for the loader.

INSTALLING. The engine looks for <gamedir>/vr/cards/<viewmodel>.card, so:

    valve/*.card    ->  <your game>/valve/vr/cards/
    gearbox/*.card  ->  <your game>/gearbox/vr/cards/

WHICH MODELS THESE WERE MEASURED FROM matters, and CENSUS.txt is the record.
Every fingerprint in the census differs - the shotgun is 32 bones in valve,
52 in valve_hd, 30 in gearbox and 32 in bshift, and no two hash alike. A card
measured from one of them declines the others rather than driving the wrong
bone, which is the whole point of binding this way.

The valve cards here were measured from the VR tree's own models. The gearbox
cards were measured from the retail Opposing Force models. If a card does not
bind, the console says so and names the fingerprint it found - re-measure with
    python vrcardgen.py --census <models dir>
and correct the `match` line.
