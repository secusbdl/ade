#pragma once
#include "GitTypes.h"
namespace ade::gitdetail {
bool parseStatus(const QByteArray &bytes, GitStatus *status);
bool parseWorktrees(const QByteArray &bytes, QList<GitWorktree> *worktrees);
bool parseDiff(const QByteArray &numstat, const QByteArray &names, bool staged, QList<GitDiffEntry> *entries);
}
