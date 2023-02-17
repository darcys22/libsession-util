#include "session/config/contacts.hpp"

#include <oxenc/hex.h>
#include <sodium/crypto_generichash_blake2b.h>

#include <variant>

#include "internal.hpp"
#include "session/config/contacts.h"
#include "session/config/error.h"
#include "session/export.h"
#include "session/types.hpp"
#include "session/util.hpp"

using namespace std::literals;
using namespace session::config;
using session::ustring_view;

namespace {

void check_session_id(std::string_view session_id) {
    if (session_id.size() != 66 || !oxenc::is_hex(session_id))
        throw std::invalid_argument{
                "Invalid pubkey: expected 66 hex digits, got " + std::to_string(session_id.size()) +
                " and/or not hex"};
}

std::string session_id_to_bytes(std::string_view session_id) {
    check_session_id(session_id);
    return oxenc::from_hex(session_id);
}

}  // namespace

LIBSESSION_C_API bool session_id_is_valid(const char* session_id) {
    return std::strlen(session_id) == 66 && oxenc::is_hex(session_id, session_id + 66);
}

contact_info::contact_info(std::string sid) : session_id{std::move(sid)} {
    check_session_id(session_id);
}

void contact_info::set_name(std::string n) {
    if (n.size() > MAX_NAME_LENGTH)
        throw std::invalid_argument{"Invalid contact name: exceeds maximum length"};
    name = std::move(n);
}

void contact_info::set_nickname(std::string n) {
    if (n.size() > MAX_NAME_LENGTH)
        throw std::invalid_argument{"Invalid contact nickname: exceeds maximum length"};
    nickname = std::move(n);
}

Contacts::Contacts(ustring_view ed25519_secretkey, std::optional<ustring_view> dumped) :
        ConfigBase{dumped} {
    load_key(ed25519_secretkey);
}

LIBSESSION_C_API int contacts_init(
        config_object** conf,
        const unsigned char* ed25519_secretkey_bytes,
        const unsigned char* dumpstr,
        size_t dumplen,
        char* error) {
    return c_wrapper_init<Contacts>(conf, ed25519_secretkey_bytes, dumpstr, dumplen, error);
}

void contact_info::load(const dict& info_dict) {
    name = maybe_string(info_dict, "n").value_or("");
    nickname = maybe_string(info_dict, "N").value_or("");

    auto url = maybe_string(info_dict, "p");
    auto key = maybe_ustring(info_dict, "q");
    if (url && key && !url->empty() && key->size() == 32) {
        profile_picture.url = std::move(*url);
        profile_picture.key = std::move(*key);
    } else {
        profile_picture.clear();
    }

    approved = maybe_int(info_dict, "a").value_or(0);
    approved_me = maybe_int(info_dict, "A").value_or(0);
    blocked = maybe_int(info_dict, "b").value_or(0);
    hidden = maybe_int(info_dict, "h").value_or(0);

    priority = maybe_int(info_dict, "+").value_or(0);
    if (priority < 0)
        priority = 0;

    int exp_mode_ = maybe_int(info_dict, "e").value_or(0);
    if (exp_mode_ >= static_cast<int>(expiration_mode::none) &&
        exp_mode_ <= static_cast<int>(expiration_mode::after_read))
        exp_mode = static_cast<expiration_mode>(exp_mode_);
    else
        exp_mode = expiration_mode::none;

    if (exp_mode == expiration_mode::none)
        exp_timer = 0min;
    else {
        int mins = maybe_int(info_dict, "E").value_or(0);
        if (mins <= 0) {
            exp_mode = expiration_mode::none;
            exp_timer = 0min;
        } else {
            exp_timer = std::chrono::minutes{mins};
        }
    }
}

static_assert(sizeof(contacts_contact::name) == contact_info::MAX_NAME_LENGTH + 1);
static_assert(sizeof(contacts_contact::nickname) == contact_info::MAX_NAME_LENGTH + 1);
static_assert(sizeof(user_profile_pic::url) == profile_pic::MAX_URL_LENGTH + 1);

void contact_info::into(contacts_contact& c) const {
    std::memcpy(c.session_id, session_id.data(), 67);
    copy_c_str(c.name, name);
    copy_c_str(c.nickname, nickname);
    if (profile_picture) {
        copy_c_str(c.profile_pic.url, profile_picture.url);
        std::memcpy(c.profile_pic.key, profile_picture.key.data(), 32);
    } else {
        copy_c_str(c.profile_pic.url, "");
    }
    c.approved = approved;
    c.approved_me = approved_me;
    c.blocked = blocked;
    c.hidden = hidden;
    c.priority = std::max(0, priority);
    c.exp_mode = static_cast<CONVO_EXPIRATION_MODE>(exp_mode);
    c.exp_minutes = exp_timer.count();
    if (c.exp_minutes <= 0 && c.exp_mode != CONVO_EXPIRATION_NONE)
        c.exp_mode = CONVO_EXPIRATION_NONE;
}

contact_info::contact_info(const contacts_contact& c) : session_id{c.session_id, 66} {
    assert(std::strlen(c.name) <= MAX_NAME_LENGTH);
    name = c.name;
    assert(std::strlen(c.nickname) <= MAX_NAME_LENGTH);
    nickname = c.nickname;
    assert(std::strlen(c.profile_pic.url) <= profile_pic::MAX_URL_LENGTH);
    if (std::strlen(c.profile_pic.url)) {
        profile_picture.url = c.profile_pic.url;
        profile_picture.key = {c.profile_pic.key, 32};
    }
    approved = c.approved;
    approved_me = c.approved_me;
    blocked = c.blocked;
    hidden = c.hidden;
    priority = std::max(0, c.priority);
    exp_mode = static_cast<expiration_mode>(c.exp_mode);
    exp_timer = exp_mode == expiration_mode::none ? 0min : std::chrono::minutes{c.exp_minutes};
    if (exp_timer <= 0min && exp_mode != expiration_mode::none)
        exp_mode = expiration_mode::none;
}

std::optional<contact_info> Contacts::get(std::string_view pubkey_hex) const {
    std::string pubkey = session_id_to_bytes(pubkey_hex);

    auto* info_dict = data["c"][pubkey].dict();
    if (!info_dict)
        return std::nullopt;

    auto result = std::make_optional<contact_info>(std::string{pubkey_hex});
    result->load(*info_dict);
    return result;
}

LIBSESSION_C_API bool contacts_get(
        const config_object* conf, contacts_contact* contact, const char* session_id) {
    try {
        if (auto c = unbox<Contacts>(conf)->get(session_id)) {
            c->into(*contact);
            return true;
        }
    } catch (...) {
    }
    return false;
}

contact_info Contacts::get_or_construct(std::string_view pubkey_hex) const {
    if (auto maybe = get(pubkey_hex))
        return *std::move(maybe);

    return contact_info{std::string{pubkey_hex}};
}

LIBSESSION_C_API bool contacts_get_or_construct(
        const config_object* conf, contacts_contact* contact, const char* session_id) {
    try {
        unbox<Contacts>(conf)->get_or_construct(session_id).into(*contact);
        return true;
    } catch (...) {
        return false;
    }
}

void Contacts::set(const contact_info& contact) {
    std::string pk = session_id_to_bytes(contact.session_id);
    auto info = data["c"][pk];

    // Always set the name, even if empty, to keep the dict from getting pruned if there are no
    // other entries.
    info["n"] = contact.name.substr(0, contact_info::MAX_NAME_LENGTH);
    set_nonempty_str(info["N"], contact.nickname.substr(0, contact_info::MAX_NAME_LENGTH));

    set_pair_if(
            contact.profile_picture,
            info["p"],
            contact.profile_picture.url,
            info["q"],
            contact.profile_picture.key);

    set_flag(info["a"], contact.approved);
    set_flag(info["A"], contact.approved_me);
    set_flag(info["b"], contact.blocked);
    set_flag(info["h"], contact.hidden);

    set_positive_int(info["+"], contact.priority);

    set_pair_if(
            contact.exp_mode != expiration_mode::none && contact.exp_timer > 0min,
            info["e"],
            static_cast<int8_t>(contact.exp_mode),
            info["E"],
            contact.exp_timer.count());
}

LIBSESSION_C_API void contacts_set(config_object* conf, const contacts_contact* contact) {
    unbox<Contacts>(conf)->set(contact_info{*contact});
}

void Contacts::set_name(std::string_view session_id, std::string name) {
    auto c = get_or_construct(session_id);
    c.set_name(std::move(name));
    set(c);
}
void Contacts::set_nickname(std::string_view session_id, std::string nickname) {
    auto c = get_or_construct(session_id);
    c.set_nickname(std::move(nickname));
    set(c);
}
void Contacts::set_profile_pic(std::string_view session_id, profile_pic pic) {
    auto c = get_or_construct(session_id);
    c.profile_picture = std::move(pic);
    set(c);
}
void Contacts::set_approved(std::string_view session_id, bool approved) {
    auto c = get_or_construct(session_id);
    c.approved = approved;
    set(c);
}
void Contacts::set_approved_me(std::string_view session_id, bool approved_me) {
    auto c = get_or_construct(session_id);
    c.approved_me = approved_me;
    set(c);
}
void Contacts::set_blocked(std::string_view session_id, bool blocked) {
    auto c = get_or_construct(session_id);
    c.blocked = blocked;
    set(c);
}

void Contacts::set_hidden(std::string_view session_id, bool hidden) {
    auto c = get_or_construct(session_id);
    c.hidden = hidden;
    set(c);
}

void Contacts::set_priority(std::string_view session_id, int priority) {
    auto c = get_or_construct(session_id);
    c.priority = priority;
    set(c);
}

void Contacts::set_expiry(
        std::string_view session_id, expiration_mode mode, std::chrono::minutes timer) {
    auto c = get_or_construct(session_id);
    c.exp_mode = mode;
    c.exp_timer = c.exp_mode == expiration_mode::none ? 0min : timer;
    set(c);
}

bool Contacts::erase(std::string_view session_id) {
    std::string pk = session_id_to_bytes(session_id);
    auto info = data["c"][pk];
    bool ret = info.exists();
    info.erase();
    return ret;
}

LIBSESSION_C_API bool contacts_erase(config_object* conf, const char* session_id) {
    try {
        return unbox<Contacts>(conf)->erase(session_id);
    } catch (...) {
        return false;
    }
}

Contacts::iterator Contacts::erase(iterator it) {
    std::string session_id = it->session_id;
    ++it;
    erase(session_id);
    return it;
}

size_t Contacts::size() const {
    if (auto* c = data["c"].dict())
        return c->size();
    return 0;
}

LIBSESSION_C_API size_t contacts_size(const config_object* conf) {
    return unbox<Contacts>(conf)->size();
}

/// Load _val from the current iterator position; if it is invalid, skip to the next key until we
/// find one that is valid (or hit the end).
void Contacts::iterator::_load_info() {
    while (_it != _contacts->end()) {
        if (_it->first.size() == 33) {
            if (auto* info_dict = std::get_if<dict>(&_it->second)) {
                _val = std::make_shared<contact_info>(oxenc::to_hex(_it->first));
                auto hex = oxenc::to_hex(_it->first);
                _val->load(*info_dict);
                return;
            }
        }

        // We found something we don't understand (wrong pubkey size, or not a dict value) so skip
        // it.
        ++_it;
    }
}

bool Contacts::iterator::operator==(const iterator& other) const {
    if (!_contacts && !other._contacts)
        return true;  // Both are end tombstones
    if (!other._contacts)
        // other is an "end" tombstone: return whether we are at the end
        return _it == _contacts->end();
    return _it == other._it;
}

bool Contacts::iterator::done() const {
    return !_contacts || _it == _contacts->end();
}

Contacts::iterator& Contacts::iterator::operator++() {
    ++_it;
    _load_info();
    return *this;
}

LIBSESSION_C_API contacts_iterator* contacts_iterator_new(const config_object* conf) {
    auto* it = new contacts_iterator{};
    it->_internals = new Contacts::iterator{unbox<Contacts>(conf)->begin()};
    return it;
}

LIBSESSION_C_API void contacts_iterator_free(contacts_iterator* it) {
    delete static_cast<Contacts::iterator*>(it->_internals);
    delete it;
}

LIBSESSION_C_API bool contacts_iterator_done(contacts_iterator* it, contacts_contact* c) {
    auto& real = *static_cast<Contacts::iterator*>(it->_internals);
    if (real.done())
        return true;
    real->into(*c);
    return false;
}

LIBSESSION_C_API void contacts_iterator_advance(contacts_iterator* it) {
    ++*static_cast<Contacts::iterator*>(it->_internals);
}

LIBSESSION_C_API void contacts_iterator_erase(config_object* conf, contacts_iterator* it) {
    auto& real = *static_cast<Contacts::iterator*>(it->_internals);
    real = unbox<Contacts>(conf)->erase(real);
}
