// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/completion/strategies/ClassicUserStrategy.hpp"

#include <algorithm>

namespace chatterino::completion {

void ClassicUserStrategy::apply(const std::vector<UserItem> &items,
                                std::vector<UserItem> &output,
                                const QString &query) const
{
    QString lowerQuery = query.toLower();
    if (lowerQuery.startsWith('@'))
    {
        lowerQuery = lowerQuery.mid(1);
    }

    for (const auto &item : items)
    {
        if (item.first.startsWith(lowerQuery))
        {
            output.push_back(item);
        }
    }

    std::ranges::sort(output, [](const UserItem &lhs, const UserItem &rhs) {
        const auto displayCompare =
            QString::compare(lhs.second, rhs.second, Qt::CaseInsensitive);
        if (displayCompare != 0)
        {
            return displayCompare < 0;
        }

        return QString::compare(lhs.first, rhs.first, Qt::CaseInsensitive) < 0;
    });
}
}  // namespace chatterino::completion
