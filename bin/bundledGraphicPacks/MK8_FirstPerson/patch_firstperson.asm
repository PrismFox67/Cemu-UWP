[MK8_FirstPerson_v42]
moduleMatches = 0x9F0A90B7 ; v4.2 (v81)

# Race camera controller update (0x02172110, r31 = controller) computes two camera outputs every frame:
#   controller+0x04: driving camera (0x02172030)    controller+0x34: look-back camera (0x02170d94)
# Output layout: +0x08 eye, +0x14 look-at point (the driver's head area, ~10 above the kart), +0x20 up, +0x2c fovy.
# The chase eye sits ~49 units behind / ~7 above the look-at point (the look-back eye ~35 in front). Move the eye to
# look-at + forward * (look-at - eye), raise it by height along the camera up vector, and keep the view direction
# (its part along up scaled by pitch: 1 = the chase camera's downward tilt, 0 = level).
# The game fades the driver out when the camera is this close, so the kart doesn't block the view.
# Split screen: every player has a race camera controller and its own state block (_fpP, see _fpSlot): mode, camera
# constants, hide centre, pitch smoothing, scratch. The players' cameras are updated on different cores at the same time,
# so the block pointer stays in a register (r8) and nothing per player is kept in shared memory.
.origin = codecave

# r3 = camera output, r8 = player block. Uses r12, f0-f13 only. d = at - eye, u = up:
#   eye' = at + forward * d + height * u,  at' = eye' + d + (pitch - 1) * (d . u) * u
_fpApply:
addi r12, r8, 128
# d
lfs f0, 20(r3)
lfs f1, 8(r3)
fsubs f1, f0, f1
lfs f0, 24(r3)
lfs f2, 12(r3)
fsubs f2, f0, f2
lfs f0, 28(r3)
lfs f3, 16(r3)
fsubs f3, f0, f3
# u
lfs f4, 32(r3)
lfs f5, 36(r3)
lfs f6, 40(r3)
# eye' into f7..f9
lfs f10, 0(r12)
lfs f11, 4(r12)
lfs f0, 20(r3)
fmadds f7, f1, f10, f0
fmadds f7, f4, f11, f7
lfs f0, 24(r3)
fmadds f8, f2, f10, f0
fmadds f8, f5, f11, f8
lfs f0, 28(r3)
fmadds f9, f3, f10, f0
fmadds f9, f6, f11, f9
stfs f7, 8(r3)
stfs f8, 12(r3)
stfs f9, 16(r3)
# k = (pitch - 1) * (d . u)
fmuls f13, f1, f4
fmadds f13, f2, f5, f13
fmadds f13, f3, f6, f13
lfs f12, 8(r12)
lfs f0, 12(r12)
fsubs f12, f12, f0
fmuls f13, f13, f12
# at' = eye' + d + k * u
fadds f7, f7, f1
fmadds f7, f4, f13, f7
fadds f8, f8, f2
fmadds f8, f5, f13, f8
fadds f9, f9, f3
fmadds f9, f6, f13, f9
stfs f7, 20(r3)
stfs f8, 24(r3)
stfs f9, 28(r3)
blr

# driving camera: 0x02172030 (r30 = controller, r31 = output) has just stored its result and calls 0x02171d6c(r3 = r30).
# Controller byte +157 set means it kept the old eye, leave that alone
_fpDrive:
mflr r11
mr r3, r30
bl _fpSlot
mr r8, r12
mtlr r11
lbz r0, 157(r30)
cmpwi r0, 0
bne _fpDriveDone
# mode 2 = the game's own camera: nothing hidden
mr r12, r8
mflr r11
bl _fpMode
mtlr r11
cmpwi r0, 2
bne _fpDriveOn
mr r12, r8
li r0, 0
stw r0, 12(r12)
b _fpDriveDone
_fpDriveOn:
mflr r11
mr r3, r31
bl _fpApply
mr r12, r8
# #511 big driver of another skeleton layout (block +160 set by _fpHideBones, Bowser, Dry Bowser): it sits higher than
# Mario and no root bone is found, so lift the cockpit eye by $fpBigUp along the camera up
lwz r0, 160(r12)
cmpwi r0, 0
beq _fpNoBig
li r0, 0
stw r0, 160(r12)
lwz r0, 156(r12)
cmpwi r0, 0
bne _fpNoBig
lis r10, _fpHideBigC@ha
addi r10, r10, _fpHideBigC@l
lfs f13, 12(r10)
lfs f0, 32(r31)
lfs f1, 8(r31)
fmadds f1, f0, f13, f1
stfs f1, 8(r31)
lfs f1, 20(r31)
fmadds f1, f0, f13, f1
stfs f1, 20(r31)
lfs f0, 36(r31)
lfs f1, 12(r31)
fmadds f1, f0, f13, f1
stfs f1, 12(r31)
lfs f1, 24(r31)
fmadds f1, f0, f13, f1
stfs f1, 24(r31)
lfs f0, 40(r31)
lfs f1, 16(r31)
fmadds f1, f0, f13, f1
stfs f1, 16(r31)
lfs f1, 28(r31)
fmadds f1, f0, f13, f1
stfs f1, 28(r31)
_fpNoBig:
# cockpit: put the eye on the driver (root bone + $fpRootUp along the camera up), so it rises with the kart when it
# hops for a drift; the view direction stays the camera's smooth one. block +64..72 = the root bone from this frame's
# bone copy (see _fpHideBones), +76 set when it was found
lwz r0, 76(r12)
cmpwi r0, 0
beq _fpNoRoot
li r0, 0
stw r0, 76(r12)
lwz r10, 156(r12)
cmpwi r10, 0
bne _fpNoRoot
lfs f13, 80(r12)
lfs f0, 32(r31)
lfs f1, 64(r12)
fmadds f1, f0, f13, f1
lfs f2, 8(r31)
fsubs f4, f1, f2
lfs f0, 36(r31)
lfs f1, 68(r12)
fmadds f1, f0, f13, f1
lfs f2, 12(r31)
fsubs f5, f1, f2
lfs f0, 40(r31)
lfs f1, 72(r12)
fmadds f1, f0, f13, f1
lfs f2, 16(r31)
fsubs f6, f1, f2
# a root bone farther than $fpRootMax from the eye the game camera gives is another driver (split screen: one frame of
# a stale hide centre could land on a neighbour's head, and the eye would then stay locked onto that kart): ignore it
fmuls f0, f4, f4
fmadds f0, f5, f5, f0
fmadds f0, f6, f6, f0
stfs f0, 124(r12)
lis r10, _fpRootMax@ha
lfs f1, _fpRootMax@l(r10)
fcmpu cr0, f0, f1
bge _fpNoRoot
lfs f3, 8(r31)
fadds f3, f3, f4
stfs f3, 8(r31)
lfs f3, 20(r31)
fadds f3, f3, f4
stfs f3, 20(r31)
lfs f3, 12(r31)
fadds f3, f3, f5
stfs f3, 12(r31)
lfs f3, 24(r31)
fadds f3, f3, f5
stfs f3, 24(r31)
lfs f3, 16(r31)
fadds f3, f3, f6
stfs f3, 16(r31)
lfs f3, 28(r31)
fadds f3, f3, f6
stfs f3, 28(r31)
# "Locked to kart": look along the kart (the root bone turned back by the seat tilt): forward F' = F*c - U*s,
# up U' = U*c + F*s, so the view spins and rolls with tricks and drifts. The driver leans forward to throw an item or
# shoot, which tips the root bone down; unless $fpLockDir is 2 ("also flips"), the up/down angle comes from the game's
# smooth camera instead: F' gets the camera direction's component along the camera up, then F' and U' are made unit
# and orthogonal again. look-at = eye + 48 * forward + lookDown * up. Block +168: +0 camera direction, +24 F', +36 U'
lwz r0, 116(r12)
addi r10, r8, 168
lfs f10, 108(r12)
lfs f11, 112(r12)
# x
lfs f0, 96(r12)
lfs f1, 84(r12)
fmuls f2, f0, f10
fnmsubs f2, f1, f11, f2
fmuls f3, f1, f10
fmadds f3, f0, f11, f3
stfs f2, 24(r10)
stfs f3, 36(r10)
lfs f4, 20(r31)
lfs f5, 8(r31)
fsubs f4, f4, f5
stfs f4, 0(r10)
# y
lfs f0, 100(r12)
lfs f1, 88(r12)
fmuls f2, f0, f10
fnmsubs f2, f1, f11, f2
fmuls f3, f1, f10
fmadds f3, f0, f11, f3
stfs f2, 28(r10)
stfs f3, 40(r10)
lfs f4, 24(r31)
lfs f5, 12(r31)
fsubs f4, f4, f5
stfs f4, 4(r10)
# z
lfs f0, 104(r12)
lfs f1, 92(r12)
fmuls f2, f0, f10
fnmsubs f2, f1, f11, f2
fmuls f3, f1, f10
fmadds f3, f0, f11, f3
stfs f2, 32(r10)
stfs f3, 44(r10)
lfs f4, 28(r31)
lfs f5, 16(r31)
fsubs f4, f4, f5
stfs f4, 8(r10)
cmpwi r0, 2
beq _fpLockWrite
addi r3, r10, 0
bl _fpNorm3
# "Steady" (0) takes the same path with forward = the camera direction and up = the camera up (no spin or roll)
mr r12, r8
lwz r0, 116(r12)
cmpwi r0, 0
bne _fpLockKart
lwz r9, 0(r10)
stw r9, 24(r10)
lwz r9, 4(r10)
stw r9, 28(r10)
lwz r9, 8(r10)
stw r9, 32(r10)
lwz r9, 32(r31)
stw r9, 36(r10)
lwz r9, 36(r31)
stw r9, 40(r10)
lwz r9, 40(r31)
stw r9, 44(r10)
_fpLockKart:
# a = F'.Ucam, b = dir.Ucam (Ucam = the camera's up, still at +32), F' += (b - a) * Ucam
lfs f0, 32(r31)
lfs f1, 36(r31)
lfs f2, 40(r31)
lfs f3, 24(r10)
fmuls f6, f3, f0
lfs f3, 28(r10)
fmadds f6, f3, f1, f6
lfs f3, 32(r10)
fmadds f6, f3, f2, f6
lfs f3, 0(r10)
fmuls f7, f3, f0
lfs f3, 4(r10)
fmadds f7, f3, f1, f7
lfs f3, 8(r10)
fmadds f7, f3, f2, f7
# the game camera dips for a moment when the driver leans to throw or hops into a drift (its look-at follows the
# driver, #15): a downward change is ignored for fpPitchHold frames (throw lean, backward throw, drift hop all end
# sooner), then b follows a smoothed copy (_fpPitchS += rate * (b - _fpPitchS)) - downhill slopes; up goes at once
addi r9, r12, 144
lfs f8, 0(r9)
lfs f5, 4(r9)
fcmpu cr0, f7, f8
bge _fpPitchRise
lwz r7, 8(r9)
addi r7, r7, 1
stw r7, 8(r9)
cmpwi r7, $fpPitchHold
blt _fpPitchKeep
fsubs f4, f7, f8
fmadds f8, f4, f5, f8
stfs f8, 0(r9)
_fpPitchKeep:
fmr f7, f8
b _fpPitchUp
_fpPitchRise:
stfs f7, 0(r9)
li r0, 0
stw r0, 8(r9)
_fpPitchUp:
fsubs f7, f7, f6
lfs f3, 24(r10)
fmadds f3, f7, f0, f3
stfs f3, 24(r10)
lfs f3, 28(r10)
fmadds f3, f7, f1, f3
stfs f3, 28(r10)
lfs f3, 32(r10)
fmadds f3, f7, f2, f3
stfs f3, 32(r10)
addi r3, r10, 24
bl _fpNorm3
# U' -= (U'.F') F', then unit length
lfs f0, 24(r10)
lfs f1, 28(r10)
lfs f2, 32(r10)
lfs f3, 36(r10)
fmuls f6, f3, f0
lfs f3, 40(r10)
fmadds f6, f3, f1, f6
lfs f3, 44(r10)
fmadds f6, f3, f2, f6
lfs f3, 36(r10)
fnmsubs f3, f6, f0, f3
stfs f3, 36(r10)
lfs f3, 40(r10)
fnmsubs f3, f6, f1, f3
stfs f3, 40(r10)
lfs f3, 44(r10)
fnmsubs f3, f6, f2, f3
stfs f3, 44(r10)
addi r3, r10, 36
bl _fpNorm3
_fpLockWrite:
lfs f12, 120(r12)
lwz r0, 116(r12)
cmpwi r0, 0
bne _fpLockTiltOn
fsubs f12, f12, f12
_fpLockTiltOn:
lis r9, _fpLockLen@ha
lfs f13, _fpLockLen@l(r9)
# x
lfs f2, 24(r10)
lfs f3, 36(r10)
stfs f3, 32(r31)
lfs f4, 8(r31)
fmadds f4, f2, f13, f4
fmadds f4, f3, f12, f4
stfs f4, 20(r31)
# y
lfs f2, 28(r10)
lfs f3, 40(r10)
stfs f3, 36(r31)
lfs f4, 12(r31)
fmadds f4, f2, f13, f4
fmadds f4, f3, f12, f4
stfs f4, 24(r31)
# z
lfs f2, 32(r10)
lfs f3, 44(r10)
stfs f3, 40(r31)
lfs f4, 16(r31)
fmadds f4, f2, f13, f4
fmadds f4, f3, f12, f4
stfs f4, 28(r31)
_fpNoRoot:
# hide centre c = eye + k * (look-at - eye), k = block +28. The bones are copied before the camera update, so the
# zone uses c + (c - previous c): one frame ahead (the kart moves several units per frame at speed). Previous c is only
# valid when the last frame was first person too (+12 set), otherwise it is c itself
lfs f13, 28(r12)
lwz r0, 12(r12)
cmpwi r0, 0
bne _fpCentre
lfs f0, 8(r31)
lfs f1, 20(r31)
fsubs f1, f1, f0
fmadds f0, f1, f13, f0
stfs f0, 48(r12)
lfs f0, 12(r31)
lfs f1, 24(r31)
fsubs f1, f1, f0
fmadds f0, f1, f13, f0
stfs f0, 52(r12)
lfs f0, 16(r31)
lfs f1, 28(r31)
fsubs f1, f1, f0
fmadds f0, f1, f13, f0
stfs f0, 56(r12)
_fpCentre:
lfs f0, 8(r31)
lfs f1, 20(r31)
fsubs f1, f1, f0
fmadds f0, f1, f13, f0
lfs f2, 48(r12)
stfs f0, 48(r12)
fadds f1, f0, f0
fsubs f1, f1, f2
stfs f1, 0(r12)
lfs f0, 12(r31)
lfs f1, 24(r31)
fsubs f1, f1, f0
fmadds f0, f1, f13, f0
lfs f2, 52(r12)
stfs f0, 52(r12)
fadds f1, f0, f0
fsubs f1, f1, f2
stfs f1, 4(r12)
lfs f0, 16(r31)
lfs f1, 28(r31)
fsubs f1, f1, f0
fmadds f0, f1, f13, f0
lfs f2, 56(r12)
stfs f0, 56(r12)
fadds f1, f0, f0
fsubs f1, f1, f2
stfs f1, 8(r12)
lwz r0, 32(r31)
stw r0, 32(r12)
lwz r0, 36(r31)
stw r0, 36(r12)
lwz r0, 40(r31)
stw r0, 40(r12)
li r0, 1
stw r0, 12(r12)
mtlr r11
_fpDriveDone:
mr r3, r30
# 0x02171d6c returns the field of view, which the caller scales and stores at +0x2c: widen it by $fpFov in the
# first person modes (the eye is low and far forward)
mr r12, r8
mflr r11
bl _fpMode
mtlr r11
cmpwi r0, 2
beq _fpDriveTail
mflr r0
stwu r1, -16(r1)
stw r0, 20(r1)
bl 0x02171d6c
lis r12, _fpFov@ha
lfs f0, _fpFov@l(r12)
fmuls f1, f1, f0
lwz r0, 20(r1)
mtlr r0
addi r1, r1, 16
blr
_fpDriveTail:
b 0x02171d6c

_fpFov:
.float $fpFov
_fpHideBigC:
.float ($fpHideBig * $fpHideBig)
.float -1000000.0
.float ($fpBigHead * $fpBigHead)
.float $fpBigUp
_fpLockLen:
.float 48.0
_fpRootMax:
.float ($fpRootMax * $fpRootMax)

# normalizes the vector of 3 floats at r3 in place (frsqrte + three Newton steps). Uses r4, f0-f8
_fpNorm3:
lfs f0, 0(r3)
lfs f1, 4(r3)
lfs f2, 8(r3)
fmuls f3, f0, f0
fmadds f3, f1, f1, f3
fmadds f3, f2, f2, f3
lis r4, _fpNormC@ha
addi r4, r4, _fpNormC@l
lfs f5, 0(r4)
lfs f6, 4(r4)
lfs f7, 8(r4)
fcmpu cr0, f3, f7
blt _fpNorm3Done
frsqrte f4, f3
fmuls f8, f4, f4
fmuls f8, f8, f3
fnmsubs f8, f8, f5, f6
fmuls f4, f4, f8
fmuls f8, f4, f4
fmuls f8, f8, f3
fnmsubs f8, f8, f5, f6
fmuls f4, f4, f8
fmuls f8, f4, f4
fmuls f8, f8, f3
fnmsubs f8, f8, f5, f6
fmuls f4, f4, f8
fmuls f0, f0, f4
fmuls f1, f1, f4
fmuls f2, f2, f4
stfs f0, 0(r3)
stfs f1, 4(r3)
stfs f2, 8(r3)
_fpNorm3Done:
blr

_fpNormC:
.float 0.5
.float 1.5
.float 0.00000001
# #787 first person only in a race: the race camera controller also runs in the menus (character and kart screens: the
# turntable driver was hidden). The racers' item slot update (0x02163f4c) runs only in a race/battle: it stamps _fpRace
# with the frame counter, and _fpMode reports the game's own camera (mode 2) when that stamp is more than 10 frames old.
_fpRace:
.int 0x80000000
# r12 = player block: r0 = its mode, or 2 outside a race. Uses r9, r10
_fpMode:
lis r10, _fpRace@ha
lwz r10, _fpRace@l(r10)
lis r9, _fpFrame@ha
lwz r9, _fpFrame@l(r9)
subf r9, r10, r9
li r0, 2
cmplwi r9, 10
bgtlr
lwz r0, 156(r12)
blr
# 0x02163f54 stw r28, 32(sp) (r0 holds the caller's lr until 0x02163f68, so it is left alone). Uses r11, r12
_fpRaceTick:
stw r28, 32(r1)
lis r12, _fpFrame@ha
lwz r11, _fpFrame@l(r12)
lis r12, _fpRace@ha
stw r11, _fpRace@l(r12)
blr

0x02163f54 = bl _fpRaceTick

0x021720e4 = bl _fpDrive

# look-back camera: 0x02170d94(r3 = controller, r4 = controller + 0x34, r5)
_fpBack:
mflr r0
stwu r1, -16(r1)
stw r0, 20(r1)
stw r4, 8(r1)
stw r3, 12(r1)
bl 0x02170d94
lwz r3, 12(r1)
bl _fpSlot
mr r8, r12
bl _fpMode
cmpwi r0, 2
beq _fpBackDone
lwz r3, 8(r1)
bl _fpApply
_fpBackDone:
lwz r0, 20(r1)
mtlr r0
addi r1, r1, 16
blr

0x0217215c = bl _fpBack

# --- Hide the driver's head ---
# Skeleton world matrices (3x4, 0x30 each) are built in the locked cache and DMA-copied to the GPU buffer by
# 0x0267e570 (bl 0x0267e364 at 0x0267e5e0: r3 = destination, r4 = locked cache source, r5 = 32-byte blocks). Before the
# copy, every matrix whose position is within $fpHide of the first-person eye gets a zero rotation, which shrinks the
# vertices bound to that bone (head, cap, eyes, moustache) to a point. The hands on the wheel are farther away.
# player 1 (216-byte blocks)
_fpP:
.float 0.0
.float 0.0
.float 0.0
.int 0
.float ($fpHide * $fpHide)
.float 0.5
.float 2.0
.float $fpHideAhead
.float 0.0
.float 1.0
.float 0.0
.float (0.0 - $fpHideBelow)
.float 0.0
.float 0.0
.float 0.0
.float 36.0
.float 0.0
.float 0.0
.float 0.0
.int 0
.float $fpRootUp
# +84 root bone up axis, +96 forward axis (this frame), +108/+112 cos/sin of the seat tilt, +116 lock, +120 look down
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.8785
.float 0.4776
.int $fpLockDir
.float $fpLockTilt
.int 0
# +124 last root distance squared (debug), +128 camera constants (forward, height, pitch, 1), +144 pitch smoothing (value, rate, frames held), +156 mode,
# +160 set when the player's driver is a big other-layout skeleton, +164 frame (GamePad read) it was last updated, +168 scratch (12 floats)
.float $fpForward
.float $fpHeight
.float $fpPitch
.float 1.0
.float 0.0
.float $fpPitchRate
.int 0
.int $fpStartMode
.int 0
.int 0x80000000
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
# player 2
.float 0.0
.float 0.0
.float 0.0
.int 0
.float ($fpHide * $fpHide)
.float 0.5
.float 2.0
.float $fpHideAhead
.float 0.0
.float 1.0
.float 0.0
.float (0.0 - $fpHideBelow)
.float 0.0
.float 0.0
.float 0.0
.float 36.0
.float 0.0
.float 0.0
.float 0.0
.int 0
.float $fpRootUp
# +84 root bone up axis, +96 forward axis (this frame), +108/+112 cos/sin of the seat tilt, +116 lock, +120 look down
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.8785
.float 0.4776
.int $fpLockDir
.float $fpLockTilt
.int 0
# +124 last root distance squared (debug), +128 camera constants (forward, height, pitch, 1), +144 pitch smoothing (value, rate, frames held), +156 mode,
# +160 set when the player's driver is a big other-layout skeleton, +164 frame (GamePad read) it was last updated, +168 scratch (12 floats)
.float $fpForward
.float $fpHeight
.float $fpPitch
.float 1.0
.float 0.0
.float $fpPitchRate
.int 0
.int $fpStartMode
.int 0
.int 0x80000000
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
# player 3
.float 0.0
.float 0.0
.float 0.0
.int 0
.float ($fpHide * $fpHide)
.float 0.5
.float 2.0
.float $fpHideAhead
.float 0.0
.float 1.0
.float 0.0
.float (0.0 - $fpHideBelow)
.float 0.0
.float 0.0
.float 0.0
.float 36.0
.float 0.0
.float 0.0
.float 0.0
.int 0
.float $fpRootUp
# +84 root bone up axis, +96 forward axis (this frame), +108/+112 cos/sin of the seat tilt, +116 lock, +120 look down
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.8785
.float 0.4776
.int $fpLockDir
.float $fpLockTilt
.int 0
# +124 last root distance squared (debug), +128 camera constants (forward, height, pitch, 1), +144 pitch smoothing (value, rate, frames held), +156 mode,
# +160 set when the player's driver is a big other-layout skeleton, +164 frame (GamePad read) it was last updated, +168 scratch (12 floats)
.float $fpForward
.float $fpHeight
.float $fpPitch
.float 1.0
.float 0.0
.float $fpPitchRate
.int 0
.int $fpStartMode
.int 0
.int 0x80000000
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
# player 4
.float 0.0
.float 0.0
.float 0.0
.int 0
.float ($fpHide * $fpHide)
.float 0.5
.float 2.0
.float $fpHideAhead
.float 0.0
.float 1.0
.float 0.0
.float (0.0 - $fpHideBelow)
.float 0.0
.float 0.0
.float 0.0
.float 36.0
.float 0.0
.float 0.0
.float 0.0
.int 0
.float $fpRootUp
# +84 root bone up axis, +96 forward axis (this frame), +108/+112 cos/sin of the seat tilt, +116 lock, +120 look down
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.8785
.float 0.4776
.int $fpLockDir
.float $fpLockTilt
.int 0
# +124 last root distance squared (debug), +128 camera constants (forward, height, pitch, 1), +144 pitch smoothing (value, rate, frames held), +156 mode,
# +160 set when the player's driver is a big other-layout skeleton, +164 frame (GamePad read) it was last updated, +168 scratch (12 floats)
.float $fpForward
.float $fpHeight
.float $fpPitch
.float 1.0
.float 0.0
.float $fpPitchRate
.int 0
.int $fpStartMode
.int 0
.int 0x80000000
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
.float 0.0
# counts the frames (GamePad reads)
_fpFrame:
.int 0

# r3 = camera controller: r12 = its player's block. Controller +148 points to the camera's screen object, +32 there
# = the screen (player) index 0-3. Uses r0, r9, r10, r12
_fpSlot:
li r10, 0
lwz r12, 148(r3)
cmpwi r12, 0
beq _fpSlotOk
lwz r10, 32(r12)
cmplwi r10, 3
ble _fpSlotOk
li r10, 3
_fpSlotOk:
mulli r10, r10, 216
lis r12, _fpP@ha
addi r12, r12, _fpP@l
add r12, r12, r10
lis r9, _fpFrame@ha
lwz r9, _fpFrame@l(r9)
stw r9, 164(r12)
blr

# every player's hide centre (split screen: the bones are shared by all views, so a hidden head is hidden for all)
_fpHideBones:
lis r7, _fpFrame@ha
lwz r7, _fpFrame@l(r7)
# #787/#794 nothing hidden outside a race: the hide flag can stay set from the attract demo while the menu camera keeps
# its old eye (controller +157), which skips the reset in _fpDrive
lis r12, _fpRace@ha
lwz r12, _fpRace@l(r12)
subf r12, r12, r7
cmplwi r12, 10
bgt _fpHideSkip
lis r12, _fpP@ha
addi r12, r12, _fpP@l
li r8, 4
_fpHideOne:
lwz r0, 12(r12)
cmpwi r0, 0
beq _fpHideDone
lwz r0, 164(r12)
subf r0, r0, r7
cmplwi r0, 10
bgt _fpHideDone
lfs f10, 0(r12)
lfs f11, 4(r12)
lfs f12, 8(r12)
lfs f13, 16(r12)
# Mario-sized driver skeletons (33 bones = 50 blocks: 0-9 body and legs, 10-16 head, 17-24 left shoulder to fingers,
# 25-32 right): when bone 10 (the head) is within 6 units of the hide centre this is the player's driver, so hide every
# bone except the arms (18-24, 26-32). That keeps the hands on the wheel in every pose; other skeletons use the distance
# rule below
cmpwi r5, 50
bne _fpHideByDistance
lfs f5, 492(r4)
fsubs f5, f5, f10
fmuls f9, f5, f5
lfs f6, 508(r4)
fsubs f6, f6, f11
fmadds f9, f6, f6, f9
lfs f7, 524(r4)
fsubs f7, f7, f12
fmadds f9, f7, f7, f9
lfs f0, 60(r12)
fcmpu cr0, f9, f0
bge _fpHideByDistance
# remember the root bone (bone 0) for the cockpit eye
lwz r0, 12(r4)
stw r0, 64(r12)
lwz r0, 28(r4)
stw r0, 68(r12)
lwz r0, 44(r4)
stw r0, 72(r12)
lwz r0, 4(r4)
stw r0, 84(r12)
lwz r0, 20(r4)
stw r0, 88(r12)
lwz r0, 36(r4)
stw r0, 92(r12)
lwz r0, 8(r4)
stw r0, 96(r12)
lwz r0, 24(r4)
stw r0, 100(r12)
lwz r0, 40(r4)
stw r0, 104(r12)
li r0, 1
stw r0, 76(r12)
# #511 DK, Wario, Waluigi share this layout but are bigger (head bone 4.6 above the root, Mario 2.6): their arms fill
# the view, so hide the whole driver (as the big drivers preset did); the kart and wheel stay
lfs f0, 492(r4)
lfs f1, 12(r4)
fsubs f0, f0, f1
fmuls f9, f0, f0
lfs f0, 508(r4)
lfs f1, 28(r4)
fsubs f0, f0, f1
fmadds f9, f0, f0, f9
lfs f0, 524(r4)
lfs f1, 44(r4)
fsubs f0, f0, f1
fmadds f9, f0, f0, f9
lis r10, _fpHideBigC@ha
addi r10, r10, _fpHideBigC@l
lfs f0, 8(r10)
li r0, 0
mr r10, r4
li r9, 33
fcmpu cr0, f9, f0
bgt _fpHideBody
li r9, (18 + $fpHideArm)
_fpHideBody:
stw r0, 0(r10)
stw r0, 4(r10)
stw r0, 8(r10)
stw r0, 16(r10)
stw r0, 20(r10)
stw r0, 24(r10)
stw r0, 32(r10)
stw r0, 36(r10)
stw r0, 40(r10)
addi r10, r10, 48
addic. r9, r9, -1
bne _fpHideBody
# right arm: bone 25 (shoulder) plus $fpHideArm more
addi r10, r4, 1200
li r9, (1 + $fpHideArm)
_fpHideArmR:
stw r0, 0(r10)
stw r0, 4(r10)
stw r0, 8(r10)
stw r0, 16(r10)
stw r0, 20(r10)
stw r0, 24(r10)
stw r0, 32(r10)
stw r0, 36(r10)
stw r0, 40(r10)
addi r10, r10, 48
addic. r9, r9, -1
bne _fpHideArmR
b _fpHideDone
_fpHideByDistance:
slwi r11, r5, 5
add r11, r4, r11
subi r11, r11, 48
# #511 other skeletons (Bowser, DK, Wario, Waluigi, Dry Bowser, Tanooki Mario...): when any bone is inside the hide radius
# this is the player's driver, so hide every bone within $fpHideBig with no floor (their arms and fists fill the view
# otherwise). f3 = radius squared, f4 = floor
fmr f3, f13
lfs f4, 44(r12)
mr r10, r4
_fpHideScan:
cmplw r10, r11
bgt _fpHideScanned
lfs f5, 12(r10)
fsubs f5, f5, f10
fmuls f9, f5, f5
lfs f6, 28(r10)
fsubs f6, f6, f11
fmadds f9, f6, f6, f9
lfs f7, 44(r10)
fsubs f7, f7, f12
fmadds f9, f7, f7, f9
addi r10, r10, 48
fcmpu cr0, f9, f13
bge _fpHideScan
lis r10, _fpHideBigC@ha
addi r10, r10, _fpHideBigC@l
lfs f3, 0(r10)
lfs f4, 4(r10)
li r0, 1
stw r0, 160(r12)
# #794 Bowser (the only 36-matrix driver) gets his own rule
cmpwi r5, 54
beq _fpHideKoopa
_fpHideScanned:
mr r10, r4
_fpHideLoop:
cmplw r10, r11
bgt _fpHideDone
lfs f5, 12(r10)
fsubs f5, f5, f10
fmuls f9, f5, f5
lfs f6, 28(r10)
fsubs f6, f6, f11
fmadds f9, f6, f6, f9
lfs f7, 44(r10)
fsubs f7, f7, f12
fmadds f9, f7, f7, f9
fcmpu cr0, f9, f3
bge _fpHideNext
# and not lower than $fpHideBelow under the centre (along the camera up vector): keeps the arms and hands
lfs f0, 32(r12)
fmuls f9, f5, f0
lfs f0, 36(r12)
fmadds f9, f6, f0, f9
lfs f0, 40(r12)
fmadds f9, f7, f0, f9
fcmpu cr0, f9, f4
ble _fpHideNext
# only rotation matrices (first row of unit-ish length)
lfs f0, 0(r10)
fmuls f8, f0, f0
lfs f0, 4(r10)
fmadds f8, f0, f0, f8
lfs f0, 8(r10)
fmadds f8, f0, f0, f8
lfs f0, 20(r12)
fcmpu cr0, f8, f0
ble _fpHideNext
lfs f0, 24(r12)
fcmpu cr0, f8, f0
bge _fpHideNext
li r0, 0
stw r0, 0(r10)
stw r0, 4(r10)
stw r0, 8(r10)
stw r0, 16(r10)
stw r0, 20(r10)
stw r0, 24(r10)
stw r0, 32(r10)
stw r0, 36(r10)
stw r0, 40(r10)
_fpHideNext:
addi r10, r10, 48
b _fpHideLoop
_fpHideDone:
addi r12, r12, 216
addic. r8, r8, -1
bne _fpHideOne
_fpHideSkip:
b 0x0267e364

# #794 Bowser: arms invisible, hands stay on the bars. Matrices are bone world matrices (+12/+28/+44 = the joint), the
# matrix index is the bone index (Koopa.bfres: 19-21 ShoulderL/ArmL/ElbowL, 22-26 HandL + fingers, 27-29 right arm,
# 30-34 right hand, 35 rigid head). Hiding a bone in place left its skin stretched between the hidden joints and the
# hands (the yellow sheets of #511), so every hidden bone gets a zero rotation AND is moved: the left arm onto the left
# wrist, the right arm onto the right wrist, body/legs/tail/head onto the point halfway between the wrists. All hidden
# vertices then lie on the line between the hands and draw nothing. Uses r0, r9, r10, f0-f9
_fpHideKoopa:
lfs f0, 1068(r4)
lfs f1, 1084(r4)
lfs f2, 1100(r4)
lfs f5, 1452(r4)
lfs f6, 1468(r4)
lfs f7, 1484(r4)
lis r10, _fpHalf@ha
lfs f9, _fpHalf@l(r10)
fadds f3, f0, f5
fmuls f3, f3, f9
fadds f4, f1, f6
fmuls f4, f4, f9
fadds f8, f2, f7
fmuls f8, f8, f9
mr r10, r4
li r9, 0
li r0, 0
_fpKpLoop:
cmpwi r9, 22
blt _fpKpHide
cmpwi r9, 26
ble _fpKpNext
cmpwi r9, 30
blt _fpKpHide
cmpwi r9, 34
ble _fpKpNext
_fpKpHide:
stw r0, 0(r10)
stw r0, 4(r10)
stw r0, 8(r10)
stw r0, 16(r10)
stw r0, 20(r10)
stw r0, 24(r10)
stw r0, 32(r10)
stw r0, 36(r10)
stw r0, 40(r10)
cmpwi r9, 19
blt _fpKpMid
cmpwi r9, 21
ble _fpKpL
cmpwi r9, 27
blt _fpKpMid
cmpwi r9, 29
ble _fpKpR
_fpKpMid:
stfs f3, 12(r10)
stfs f4, 28(r10)
stfs f8, 44(r10)
b _fpKpNext
_fpKpL:
stfs f0, 12(r10)
stfs f1, 28(r10)
stfs f2, 44(r10)
b _fpKpNext
_fpKpR:
stfs f5, 12(r10)
stfs f6, 28(r10)
stfs f7, 44(r10)
_fpKpNext:
addi r10, r10, 48
addi r9, r9, 1
cmpwi r9, 36
blt _fpKpLoop
b _fpHideDone
_fpHalf:
.float 0.5

0x0267e5e0 = bl _fpHideBones

# --- Camera button: right stick click (R3) cycles cockpit -> first person without kart -> the game's own camera ---
# The game reads the GamePad once per frame: 0x02725720 VPADRead(0, r31 + 20, 16, r31 + 0xad8), the sample count goes to
# r31 + 0xad4. Sample 0 is the newest: +0 held buttons, +4 newly pressed. 0x00020000 = right stick click.
# Mode 0 uses this preset's values, mode 1 the first person view without the kart, mode 2 turns the pack off.
# The GamePad is player 1; players 2-4 are Pro Controllers on KPAD channels 0-2 (see _fpKpad).
_fpModes:
.float $fpForward
.float $fpHeight
.float $fpPitch
.float ($fpHide * $fpHide)
.float $fpHideAhead
.float (0.0 - $fpHideBelow)
.float 0.12
.float 0.0
.float 0.3
.float 0.0
.float 0.0
.float -2.0

_fpPad:
stw r3, 2772(r31)
lis r12, _fpFrame@ha
lwz r11, _fpFrame@l(r12)
addi r11, r11, 1
stw r11, _fpFrame@l(r12)
cmpwi r3, 1
blt _fpPadDone
lwz r0, 24(r31)
andis. r0, r0, 2
beq _fpPadDone
lis r12, _fpP@ha
addi r12, r12, _fpP@l
b _fpCycle
_fpPadDone:
blr

# r12 = player block: next mode, and copy the mode's row into its camera constants and hide settings. Uses r0, r10, r11
_fpCycle:
lwz r11, 156(r12)
addi r11, r11, 1
cmpwi r11, 3
blt _fpCycleStore
li r11, 0
_fpCycleStore:
stw r11, 156(r12)
cmpwi r11, 2
blt _fpCycleRow
blr
_fpCycleRow:
lis r10, _fpModes@ha
addi r10, r10, _fpModes@l
mulli r11, r11, 24
add r10, r10, r11
lwz r0, 0(r10)
stw r0, 128(r12)
lwz r0, 4(r10)
stw r0, 132(r12)
lwz r0, 8(r10)
stw r0, 136(r12)
lwz r0, 12(r10)
stw r0, 16(r12)
lwz r0, 16(r10)
stw r0, 28(r12)
lwz r0, 20(r10)
stw r0, 44(r12)
blr

0x02725724 = bl _fpPad

# Pro Controllers: 0x02724af4 loops over KPAD channels 0-3, KPADReadEx(channel, r29, 16, r29 + 3844) with r29 = r30 +
# channel * 3848 and the sample count to r29 + 3840. KPADStatus +92 extension type (31 = Pro Controller), +100 the Pro
# Controller's newly pressed buttons (0x00020000 = right stick click)
_fpKpad:
stw r3, 3840(r29)
cmpwi r3, 1
blt _fpKpadDone
lbz r0, 92(r29)
cmpwi r0, 31
bne _fpKpadDone
lwz r0, 100(r29)
andis. r0, r0, 2
beq _fpKpadDone
subf r11, r30, r29
lis r12, _fpP@ha
addi r12, r12, _fpP@l
addi r12, r12, 216
cmplwi r11, 3848
blt _fpCycle
addi r12, r12, 216
cmplwi r11, 7696
blt _fpCycle
addi r12, r12, 216
cmplwi r11, 11544
blt _fpCycle
_fpKpadDone:
blr

0x02724b24 = bl _fpKpad
