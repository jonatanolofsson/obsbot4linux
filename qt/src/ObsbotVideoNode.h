// Finding the camera's V4L2 capture node.
//
// There is exactly one right way to do this and it is NOT "/dev/video0". That
// assumption holds only on a machine where the OBSBOT is the sole camera; on a
// laptop /dev/video0 is the built-in webcam, and the ffplay fallback duly opened
// the wrong camera. The Tiny 3 also presents TWO nodes (e.g. video4 + video5)
// and only one of them is a capture node, so "the node whose card name matches"
// is not sufficient either — the capability bit has to be checked too.
//
// This is PreviewEngine's original scan, lifted out so the embedded preview and
// the ffplay fallback resolve the device identically instead of one of them
// guessing.
#pragma once

#include <QString>

namespace ObsbotVideoNode {

// Returns the first /dev/video* CAPTURE node whose driver-reported card name
// contains "OBSBOT", or an empty string when none is present. Nodes are visited
// in numeric order so the pick is stable across boots.
//
// Opens each candidate briefly to query it; a node that cannot be opened (busy,
// permissions) is skipped rather than treated as a match.
QString find();

} // namespace ObsbotVideoNode
