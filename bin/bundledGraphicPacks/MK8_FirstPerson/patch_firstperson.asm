[MK8_FirstPerson_v42]
moduleMatches = 0x9F0A90B7 ; v4.2 (v81)

# Race camera controller update (0x02172110, r31 = controller) computes two camera outputs every frame:
#   controller+0x04: driving camera (0x02172030)    controller+0x34: look-back camera (0x02170d94)
# Output layout: +0x08 eye, +0x14 look-at point (the driver's head area, ~10 above the kart), +0x20 up, +0x2c fovy.
# The chase eye sits ~49 units behind / ~7 above the look-at point (the look-back eye ~35 in front). Move the eye to
# look-at + forward * (look-at - eye), raise it by height along the camera up vector, and keep the view direction
# (its part along up scaled by pitch: 1 = the chase camera's downward tilt, 0 = level).
# The game fades the driver out when the camera is this close, so the kart doesn't block the view.
.origin = codecave
_fpConst:
.float $fpForward
.float $fpHeight
.float $fpPitch
.float 1.0

# r3 = camera output. Uses r12, f0-f13 only. d = at - eye, u = up:
#   eye' = at + forward * d + height * u,  at' = eye' + d + (pitch - 1) * (d . u) * u
_fpApply:
lis r12, _fpConst@ha
addi r12, r12, _fpConst@l
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
lbz r0, 157(r30)
cmpwi r0, 0
bne _fpDriveDone
# mode 2 = the game's own camera: nothing hidden
lis r12, _fpMode@ha
lwz r0, _fpMode@l(r12)
cmpwi r0, 2
bne _fpDriveOn
lis r12, _fpEye@ha
addi r12, r12, _fpEye@l
li r0, 0
stw r0, 12(r12)
b _fpDriveDone
_fpDriveOn:
mflr r11
mr r3, r31
bl _fpApply
lis r12, _fpEye@ha
addi r12, r12, _fpEye@l
# cockpit: put the eye on the driver (root bone + $fpRootUp along the camera up), so it rises with the kart when it
# hops for a drift; the view direction stays the camera's smooth one. _fpEye+64..72 = the root bone from this frame's
# bone copy (see _fpHideBones), +76 set when it was found
lwz r0, 76(r12)
cmpwi r0, 0
beq _fpNoRoot
li r0, 0
stw r0, 76(r12)
lis r10, _fpMode@ha
lwz r10, _fpMode@l(r10)
cmpwi r10, 0
bne _fpNoRoot
lfs f13, 80(r12)
lfs f0, 32(r31)
lfs f1, 64(r12)
fmadds f1, f0, f13, f1
lfs f2, 8(r31)
fsubs f2, f1, f2
stfs f1, 8(r31)
lfs f3, 20(r31)
fadds f3, f3, f2
stfs f3, 20(r31)
lfs f0, 36(r31)
lfs f1, 68(r12)
fmadds f1, f0, f13, f1
lfs f2, 12(r31)
fsubs f2, f1, f2
stfs f1, 12(r31)
lfs f3, 24(r31)
fadds f3, f3, f2
stfs f3, 24(r31)
lfs f0, 40(r31)
lfs f1, 72(r12)
fmadds f1, f0, f13, f1
lfs f2, 16(r31)
fsubs f2, f1, f2
stfs f1, 16(r31)
lfs f3, 28(r31)
fadds f3, f3, f2
stfs f3, 28(r31)
# "Locked to kart": look along the kart (the root bone turned back by the seat tilt): forward F' = F*c - U*s,
# up U' = U*c + F*s, so the view spins and rolls with tricks and drifts. The driver leans forward to throw an item or
# shoot, which tips the root bone down; unless $fpLockDir is 2 ("also flips"), the up/down angle comes from the game's
# smooth camera instead: F' gets the camera direction's component along the camera up, then F' and U' are made unit
# and orthogonal again. look-at = eye + 48 * forward + lookDown * up. _fpTmp: +0 camera direction, +24 F', +36 U'
lwz r0, 116(r12)
cmpwi r0, 0
beq _fpNoRoot
lis r10, _fpTmp@ha
addi r10, r10, _fpTmp@l
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
# hide centre c = eye + k * (look-at - eye), k = _fpEye+28. The bones are copied before the camera update, so the
# zone uses c + (c - previous c): one frame ahead (the kart moves several units per frame at speed)
lfs f13, 28(r12)
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
lis r12, _fpMode@ha
lwz r0, _fpMode@l(r12)
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
_fpLockLen:
.float 48.0

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
_fpTmp:
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

0x021720e4 = bl _fpDrive

# look-back camera: 0x02170d94(r3 = controller, r4 = controller + 0x34, r5)
_fpBack:
mflr r0
stwu r1, -16(r1)
stw r0, 20(r1)
stw r4, 8(r1)
bl 0x02170d94
lis r12, _fpMode@ha
lwz r0, _fpMode@l(r12)
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
_fpEye:
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

_fpHideBones:
lis r12, _fpEye@ha
addi r12, r12, _fpEye@l
lwz r0, 12(r12)
cmpwi r0, 0
beq _fpHideDone
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
li r0, 0
mr r10, r4
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
fcmpu cr0, f9, f13
bge _fpHideNext
# and not lower than $fpHideBelow under the centre (along the camera up vector): keeps the arms and hands
lfs f0, 32(r12)
fmuls f9, f5, f0
lfs f0, 36(r12)
fmadds f9, f6, f0, f9
lfs f0, 40(r12)
fmadds f9, f7, f0, f9
lfs f0, 44(r12)
fcmpu cr0, f9, f0
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
b 0x0267e364

0x0267e5e0 = bl _fpHideBones

# --- Camera button: right stick click (R3) cycles cockpit -> first person without kart -> the game's own camera ---
# The game reads the GamePad once per frame: 0x02725720 VPADRead(0, r31 + 20, 16, r31 + 0xad8), the sample count goes to
# r31 + 0xad4. Sample 0 is the newest: +0 held buttons, +4 newly pressed. 0x00020000 = right stick click.
# Mode 0 uses this preset's values, mode 1 the first person view without the kart, mode 2 turns the pack off.
_fpMode:
.int $fpStartMode
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
cmpwi r3, 1
blt _fpPadDone
lwz r0, 24(r31)
andis. r0, r0, 2
beq _fpPadDone
lis r12, _fpMode@ha
addi r12, r12, _fpMode@l
lwz r11, 0(r12)
addi r11, r11, 1
cmpwi r11, 3
blt _fpPadStore
li r11, 0
_fpPadStore:
stw r11, 0(r12)
cmpwi r11, 2
bge _fpPadDone
# copy the mode's row into the camera constants and the hide settings
mulli r10, r11, 24
add r10, r10, r12
lis r11, _fpConst@ha
addi r11, r11, _fpConst@l
lwz r0, 4(r10)
stw r0, 0(r11)
lwz r0, 8(r10)
stw r0, 4(r11)
lwz r0, 12(r10)
stw r0, 8(r11)
lis r11, _fpEye@ha
addi r11, r11, _fpEye@l
lwz r0, 16(r10)
stw r0, 16(r11)
lwz r0, 20(r10)
stw r0, 28(r11)
lwz r0, 24(r10)
stw r0, 44(r11)
_fpPadDone:
blr

0x02725724 = bl _fpPad
