////////////////////////////////////////////////////////////////////////////
//
// Copyright 2016 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#include <realm/object-store/collection_notifications.hpp>

#include <realm/object-store/impl/collection_notifier.hpp>
#include <utility>

using namespace realm;
using namespace realm::_impl;

NotificationToken::NotificationToken(std::shared_ptr<_impl::CollectionNotifier> notifier, uint64_t token)
    : m_notifier(std::move(notifier))
    , m_token(token)
{
}

NotificationToken::~NotificationToken()
{
    unregister();
}

NotificationToken::NotificationToken(NotificationToken&&) = default;

NotificationToken& NotificationToken::operator=(realm::NotificationToken&& rgt)
{
    if (this != &rgt) {
        unregister();
        m_notifier = std::move(rgt.m_notifier);
        m_token = rgt.m_token;
    }
    return *this;
}

void NotificationToken::unregister()
{
    // m_notifier itself (and not just the pointed-to thing) needs to be accessed
    // atomically to ensure that there are no data races when the token is
    // destroyed after being modified on a different thread.
    // This is needed despite the token not being thread-safe in general as
    // users find it very surprising for obj-c objects to care about what
    // thread they are deallocated on.
    if (auto notifier = m_notifier.exchange({})) {
        notifier->remove_callback(m_token);
    }
}

void NotificationToken::suppress_next()
{
    m_notifier.load()->suppress_next_notification(m_token);
}
