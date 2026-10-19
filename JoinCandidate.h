#pragma once

#include <QString>

#include "ForeignKey.h"

// A table that could be JOINed to the query being built, together with
// the foreign key that would connect it. Computed by
// Schema::joinCandidates(); consumed by the diagram (dashed nodes) and
// by the "+ Add step > JOIN" dialog, so both always offer the same set.
struct JoinCandidate
{
    QString table;        // the table that would be added to the query
    QString anchorTable;  // the table already in the query it connects to
    ForeignKey relation;  // the foreign key used for the ON clause
};
