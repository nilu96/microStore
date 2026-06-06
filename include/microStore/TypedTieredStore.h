/*
 * Copyright (c) 2026 Chad Attermann
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */

#pragma once

#include "Codec.h"
#include "FileStore.h"

#include <stdint.h>
#include <stddef.h>
#include <array>
#include <functional>
#include <vector>

namespace microStore {

#ifndef USTORE_L1_CACHE_SIZE
#define USTORE_L1_CACHE_SIZE 10
#endif

#ifndef USTORE_L1_HASH_BUCKETS
#if USTORE_L1_CACHE_SIZE <= 5
#define USTORE_L1_HASH_BUCKETS 11
#elif USTORE_L1_CACHE_SIZE <= 10
#define USTORE_L1_HASH_BUCKETS 23
#elif USTORE_L1_CACHE_SIZE <= 20
#define USTORE_L1_HASH_BUCKETS 47
#elif USTORE_L1_CACHE_SIZE <= 50
#define USTORE_L1_HASH_BUCKETS 101
#elif USTORE_L1_CACHE_SIZE <= 100
#define USTORE_L1_HASH_BUCKETS 211
#else
#define USTORE_L1_HASH_BUCKETS (USTORE_L1_CACHE_SIZE * 2 + 1)
#endif
#endif

template<
	typename Key,
	typename Value,
	typename KeyHash,
	typename Store = FileStore,
	typename KeyCodec = Codec<Key>,
	typename ValueCodec = Codec<Value>
>
class TypedTieredStore
{
public:

	struct Entry
	{
		Key key;
		Value value;
	};

	TypedTieredStore(Store& b) : store(b)
	{
		reset_l1_();
	}

	inline bool isValid() const { return store.isValid(); }
	inline operator bool() const { return isValid(); }

	bool put(const Key& key, const Value& value, uint32_t ttl, uint8_t priority)
	{
		if (!isValid()) return false;
		prune_expired_l1_();

		auto raw_key = KeyCodec::encode(key);

		int16_t l1 = find_l1_(key);
		if (l1 >= 0) {
			Slot& s = slots_[l1];
			s.value = value;
			s.expires_at = expires_at_(ttl);
			s.priority = priority;
			s.dirty = true;
			touch_l1_((uint16_t)l1);
			return true;
		}

		int16_t slot = allocate_l1_slot_();
		if (slot < 0) return false;

		Slot& s = slots_[(uint16_t)slot];
		s.key = key;
		s.value = value;
		s.expires_at = expires_at_(ttl);
		s.priority = priority;
		s.dirty = true;
		s.valid = true;
		s.in_l2 = store.exists(raw_key);
		link_l1_hash_((uint16_t)slot);
		link_lru_front_((uint16_t)slot);

		return true;
	}

	bool get(const Key& key, Value& value)
	{
		if (!isValid()) return false;

		int16_t l1 = find_l1_(key);
		if (l1 >= 0) {
			if (is_expired_(slots_[(uint16_t)l1])) {
				expire_l1_slot_((uint16_t)l1);
				return false;
			}
			value = slots_[(uint16_t)l1].value;
			touch_l1_((uint16_t)l1);
			return true;
		}

		auto raw_key = KeyCodec::encode(key);
		std::vector<uint8_t> raw_value;
		if (!store.get(raw_key, raw_value)) return false;

		Value decoded;
		if (!ValueCodec::decode(raw_value, decoded)) return false;

		int16_t slot = allocate_l1_slot_();
		if (slot >= 0) {
			Slot& s = slots_[(uint16_t)slot];
			s.key = key;
			s.value = decoded;
			s.dirty = false;
			s.valid = true;
			s.in_l2 = true;
			link_l1_hash_((uint16_t)slot);
			link_lru_front_((uint16_t)slot);
		}

		value = decoded;
		return true;
	}

	bool remove(const Key& key)
	{
		if (!isValid()) return false;

		int16_t l1 = find_l1_(key);
		bool in_l2 = false;
		if (l1 >= 0) {
			in_l2 = slots_[(uint16_t)l1].in_l2;
			remove_l1_slot_((uint16_t)l1);
		}
		else {
			auto raw_key = KeyCodec::encode(key);
			in_l2 = store.exists(raw_key);
		}

		if (in_l2) {
			auto raw_key = KeyCodec::encode(key);
			return store.remove(raw_key);
		}

		return true;
	}

	bool exists(const Key& key)
	{
		if (!isValid()) return false;
		int16_t l1 = find_l1_(key);
		if (l1 >= 0) {
			if (is_expired_(slots_[(uint16_t)l1])) {
				expire_l1_slot_((uint16_t)l1);
				return false;
			}
			return true;
		}
		auto raw_key = KeyCodec::encode(key);
		return store.exists(raw_key);
	}

	size_t size() const
	{
		if (!isValid()) return 0;
		//prune_expired_l1_();

		size_t n = store.size();
		for (uint16_t i = 0; i < USTORE_L1_CACHE_SIZE; i++) {
			if (slots_[i].valid && !slots_[i].in_l2)
				n++;
		}
		return n;
	}

	bool sync()
	{
		if (!isValid()) return false;
		prune_expired_l1_();

		bool ok = true;
		for (uint16_t i = 0; i < USTORE_L1_CACHE_SIZE; i++) {
			if (!slots_[i].valid || !slots_[i].dirty) continue;
			if (slots_[i].in_l2 && !l2_exists_(slots_[i])) {
				remove_l1_slot_(i);
				continue;
			}
			if (!flush_l1_slot_(i))
				ok = false;
		}
		return ok;
	}

	void clear()
	{
		if (!isValid()) return;
		reset_l1_();
		store.clear();
	}

	size_t cacheSize() const
	{
		size_t n = 0;
		for (uint16_t i = 0; i < USTORE_L1_CACHE_SIZE; i++)
			if (slots_[i].valid) n++;
		return n;
	}

	size_t dirtyCount() const
	{
		size_t n = 0;
		for (uint16_t i = 0; i < USTORE_L1_CACHE_SIZE; i++)
			if (slots_[i].valid && slots_[i].dirty) n++;
		return n;
	}

	class iterator
	{
	public:

		iterator(TypedTieredStore* parent, bool at_end)
			: parent_(parent), at_end_(at_end)
		{
			if (!parent_ || at_end_ || !parent_->isValid()) {
				at_end_ = true;
				return;
			}

			parent_->prune_expired_l1_();
			l2_it_ = parent_->store.begin();
			l2_end_ = parent_->store.end();
			l1_slot_ = 0;
			load_next_();
		}

		iterator& operator++()
		{
			if (at_end_) return *this;
			if (phase_ == Phase::L2)
				++l2_it_;
			else
				++l1_slot_;
			load_next_();
			return *this;
		}

		bool operator!=(const iterator& other) const
		{
			if (parent_ != other.parent_) return true;
			if (at_end_ || other.at_end_) return at_end_ != other.at_end_;
			if (phase_ != other.phase_) return true;
			if (phase_ == Phase::L2) return l2_it_ != other.l2_it_;
			return l1_slot_ != other.l1_slot_;
		}

		Entry& operator*()
		{
			return current_;
		}

	private:
		enum class Phase { L2, L1 };

		TypedTieredStore* parent_ = nullptr;
		typename Store::iterator l2_it_;
		typename Store::iterator l2_end_;
		uint16_t l1_slot_ = 0;
		Phase phase_ = Phase::L2;
		bool at_end_ = true;
		Entry current_;

		void load_next_()
		{
			while (phase_ == Phase::L2) {
				if (l2_it_ == l2_end_) {
					phase_ = Phase::L1;
					break;
				}

				Key key;
				if (!KeyCodec::decode(l2_it_->key, key)) {
					++l2_it_;
					continue;
				}

				int16_t l1 = parent_->find_l1_(key);
				if (l1 >= 0) {
					Slot& s = parent_->slots_[(uint16_t)l1];
					current_ = {s.key, s.value};
					at_end_ = false;
					return;
				}

				const auto& raw = *l2_it_;
				Value value;
				if (!ValueCodec::decode(raw.value, value)) {
					++l2_it_;
					continue;
				}
				current_ = {key, value};
				at_end_ = false;
				return;
			}

			while (l1_slot_ < USTORE_L1_CACHE_SIZE) {
				Slot& s = parent_->slots_[l1_slot_];
				if (s.valid && !s.in_l2) {
					current_ = {s.key, s.value};
					at_end_ = false;
					return;
				}
				++l1_slot_;
			}

			at_end_ = true;
		}
	};

	iterator begin()
	{
		return iterator(this, false);
	}

	iterator end()
	{
		return iterator(this, true);
	}

private:

	static const uint16_t INVALID = 0xFFFF;

	struct Slot
	{
		Key key;
		Value value;
		uint32_t expires_at = 0;
		uint8_t priority = 0;
		uint16_t prev = INVALID;
		uint16_t next = INVALID;
		uint16_t hash_next = INVALID;
		bool valid = false;
		bool dirty = false;
		bool in_l2 = false;
	};

	void reset_l1_()
	{
		for (uint16_t i = 0; i < USTORE_L1_CACHE_SIZE; i++) {
			slots_[i] = Slot{};
		}
		lru_head_ = INVALID;
		lru_tail_ = INVALID;
		for (uint16_t i = 0; i < USTORE_L1_HASH_BUCKETS; i++)
			l1_buckets_[i] = INVALID;
	}

	size_t hash_key_(const Key& key) const
	{
		return key_hash_(key);
	}

	uint32_t expires_at_(uint32_t ttl) const
	{
		if (ttl == 0) return 0;
		return microStore::time() + ttl;
	}

	bool is_expired_(const Slot& s) const
	{
		return s.expires_at != 0 && microStore::time() >= s.expires_at;
	}

	uint32_t remaining_ttl_(const Slot& s) const
	{
		if (s.expires_at == 0) return 0;
		uint32_t now = microStore::time();
		return (s.expires_at > now) ? (s.expires_at - now) : 1;
	}

	bool l2_exists_(const Slot& s)
	{
		auto raw_key = KeyCodec::encode(s.key);
		return store.exists(raw_key);
	}

	uint16_t l1_bucket_(const Key& key) const
	{
		return (uint16_t)(hash_key_(key) % USTORE_L1_HASH_BUCKETS);
	}

	int16_t find_l1_(const Key& key) const
	{
		uint16_t idx = l1_buckets_[l1_bucket_(key)];
		while (idx != INVALID) {
			const Slot& s = slots_[idx];
			if (s.valid && std::equal_to<Key>()(s.key, key))
				return (int16_t)idx;
			idx = s.hash_next;
		}
		return -1;
	}

	void link_l1_hash_(uint16_t slot)
	{
		uint16_t bucket = l1_bucket_(slots_[slot].key);
		slots_[slot].hash_next = l1_buckets_[bucket];
		l1_buckets_[bucket] = slot;
	}

	void unlink_l1_hash_(uint16_t slot)
	{
		uint16_t bucket = l1_bucket_(slots_[slot].key);
		uint16_t idx = l1_buckets_[bucket];
		uint16_t prev = INVALID;
		while (idx != INVALID) {
			if (idx == slot) {
				if (prev == INVALID)
					l1_buckets_[bucket] = slots_[idx].hash_next;
				else
					slots_[prev].hash_next = slots_[idx].hash_next;
				slots_[idx].hash_next = INVALID;
				return;
			}
			prev = idx;
			idx = slots_[idx].hash_next;
		}
	}

	void link_lru_front_(uint16_t slot)
	{
		slots_[slot].prev = INVALID;
		slots_[slot].next = lru_head_;
		if (lru_head_ != INVALID)
			slots_[lru_head_].prev = slot;
		lru_head_ = slot;
		if (lru_tail_ == INVALID)
			lru_tail_ = slot;
	}

	void unlink_lru_(uint16_t slot)
	{
		uint16_t prev = slots_[slot].prev;
		uint16_t next = slots_[slot].next;
		if (prev != INVALID)
			slots_[prev].next = next;
		else
			lru_head_ = next;
		if (next != INVALID)
			slots_[next].prev = prev;
		else
			lru_tail_ = prev;
		slots_[slot].prev = INVALID;
		slots_[slot].next = INVALID;
	}

	void touch_l1_(uint16_t slot)
	{
		if (slot == lru_head_) return;
		unlink_lru_(slot);
		link_lru_front_(slot);
	}

	int16_t free_l1_slot_() const
	{
		for (uint16_t i = 0; i < USTORE_L1_CACHE_SIZE; i++)
			if (!slots_[i].valid) return (int16_t)i;
		return -1;
	}

	int16_t allocate_l1_slot_()
	{
		int16_t free_slot = free_l1_slot_();
		if (free_slot >= 0) return free_slot;
		if (lru_tail_ == INVALID) return -1;
		uint16_t evict = lru_tail_;
		if (is_expired_(slots_[evict])) {
			expire_l1_slot_(evict);
			return (int16_t)evict;
		}
		if (!flush_l1_slot_(evict)) return -1;
		remove_l1_slot_(evict);
		return (int16_t)evict;
	}

	bool flush_l1_slot_(uint16_t slot)
	{
		Slot& s = slots_[slot];
		if (!s.valid || !s.dirty) return true;

		auto raw_key = KeyCodec::encode(s.key);
		auto raw_value = ValueCodec::encode(s.value);
		if (!store.put(raw_key, raw_value, remaining_ttl_(s), microStore::time(), s.priority))
			return false;

		s.dirty = false;
		s.in_l2 = true;
		return true;
	}

	void remove_l1_slot_(uint16_t slot)
	{
		Slot& s = slots_[slot];
		if (!s.valid) return;

		unlink_l1_hash_(slot);
		unlink_lru_(slot);
		s = Slot{};
	}

	void expire_l1_slot_(uint16_t slot)
	{
		bool in_l2 = slots_[slot].in_l2;
		Key key = slots_[slot].key;
		remove_l1_slot_(slot);
		if (in_l2) {
			auto raw_key = KeyCodec::encode(key);
			store.remove(raw_key);
		}
	}

	void prune_expired_l1_()
	{
		for (uint16_t i = 0; i < USTORE_L1_CACHE_SIZE; i++) {
			if (slots_[i].valid && is_expired_(slots_[i]))
				expire_l1_slot_(i);
		}
	}

	Store& store;
	KeyHash key_hash_;

	std::array<Slot, USTORE_L1_CACHE_SIZE> slots_;
	std::array<uint16_t, USTORE_L1_HASH_BUCKETS> l1_buckets_;
	uint16_t lru_head_ = INVALID;
	uint16_t lru_tail_ = INVALID;
};

} // namespace microStore
