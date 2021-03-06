#ifndef XCDAT_TRIE_HPP_
#define XCDAT_TRIE_HPP_

#include <string_view>
#include <xcdat/Trie.hpp>

#include "Trie.hpp"
#include "DacBc.hpp"
#include "FastDacBc.hpp"

namespace xcdat {

// Compressed string dictionary using an improved double-array trie. There are
// two versions of DACs to represent BASE/CHECK arrays in small space. The
// versions can be chosen using the Fast parameter.
template<bool Fast>
class Trie {
public:
  using trie_type = Trie<Fast>;
  using bc_type = typename std::conditional<Fast, FastDacBc, DacBc>::type;

  static constexpr auto NOT_FOUND = ID_MAX;

  // Generic constructor.
  Trie() = default;

  // Reads the dictionary from an std::istream.
  explicit Trie(std::istream& is) {
    bc_ = bc_type(is);
    terminal_flags_ = BitVector(is);
    tail_ = Vector<char>(is);
    boundary_flags_ = BitVector(is);
    alphabet_ = Vector<uint8_t>(is);
    is.read(reinterpret_cast<char*>(table_), 512);
    num_keys_ = read_value<size_t>(is);
    max_length_ = read_value<size_t>(is);
    bin_mode_ = read_value<bool>(is);
  }

  // Generic destructor.
  ~Trie() = default;

  // Lookups the ID of a given key. If the key is not registered, otherwise
  // returns NOT_FOUND.
  id_type lookup(std::string_view key) const {
    size_t pos = 0;
    id_type node_id = 0;

    while (!bc_.is_leaf(node_id)) {
      if (pos == key.length()) {
        return terminal_flags_[node_id] ? to_key_id_(node_id) : NOT_FOUND;
      }

      const auto child_id = bc_.base(node_id) ^code_(key[pos++]);
      if (bc_.check(child_id) != node_id) {
        return NOT_FOUND;
      }

      node_id = child_id;
    }

    size_t tail_pos = bc_.link(node_id);
    if (!match_suffix_(key, pos, tail_pos)) {
      return NOT_FOUND;
    }

    return to_key_id_(node_id);
  }

  // Decodes the key associated with a given ID.
  std::string access(id_type id) const {
    if (num_keys_ <= id) {
      return {};
    }

    std::string dec;
    dec.reserve(max_length_);

    auto node_id = to_node_id_(id);
    auto tail_pos = bc_.is_leaf(node_id) ? bc_.link(node_id) : NOT_FOUND;

    while (node_id) {
      const auto parent_id = bc_.check(node_id);
      dec += edge_(parent_id, node_id);
      node_id = parent_id;
    }

    std::reverse(std::begin(dec), std::end(dec));

    if (tail_pos != 0 && tail_pos != NOT_FOUND) {
      if (bin_mode_) {
        do {
          dec += tail_[tail_pos];
        } while (!boundary_flags_[tail_pos++]);
      } else {
        do {
          dec += tail_[tail_pos++];
        } while (tail_[tail_pos]);
      }
    }

    return dec;
  }

  // Iterator for enumerating the keys and IDs included as prefixes of a given
  // key, that is, supporting so-called common prefix lookup. It is created by
  // using make_prefix_iterator().
  class PrefixIterator {
  public:
    PrefixIterator() = default;

    // Scans the next key. If it does not exist, returns false.
    bool next() {
      return trie_ != nullptr && trie_->next_prefix_(this);
    }

    // Gets the key.
    std::string_view key() const {
      return {key_.data(), pos_};
    };
    // Gets the ID.
    id_type id() const {
      return id_;
    }

  private:
    const trie_type* trie_{};
    const std::string_view key_{};

    size_t pos_{0};
    id_type node_id_{0};
    id_type id_{};

    bool begin_flag_{true};
    bool end_flag_{false};

    PrefixIterator(const trie_type* trie, std::string_view key)
      : trie_{trie}, key_{key} {}

    friend class Trie;
  };

  // Makes PrefixIterator from a given key.
  PrefixIterator make_prefix_iterator(std::string_view key) const {
    return PrefixIterator{this, key};
  }

  // Iterator class for enumerating the keys and IDs starting with prefixes of
  // a given key, that is, supporting so-called predictive lookup. It is in
  // lexicographical order. It is created by using make_predictive_iterator().
  class PredictiveIterator {
  public:
    PredictiveIterator() = default;

    // Scans the next key. If it does not exist, returns false.
    bool next() {
      return trie_ != nullptr && trie_->next_predictive_(this);
    }

    // Gets the key.
    std::string_view key() const {
      return {buf_.data(), buf_.size()};
    };
    // Gets the ID.
    id_type id() const {
      return id_;
    }

  private:
    const trie_type* trie_{};
    const std::string_view key_{};

    bool begin_flag_{true};
    bool end_flag_{false};

    struct stack_t {
      size_t depth;
      char c;
      id_type node_id;
    };

    std::vector<stack_t> stack_{};
    std::string buf_{};
    id_type id_{};

    PredictiveIterator(const trie_type* trie, std::string_view key)
      : trie_{trie}, key_{key} {
      buf_.reserve(trie->max_length_);
    }

    friend class Trie;
  };

  // Makes PredictiveIterator from a given key.
  PredictiveIterator make_predictive_iterator(std::string_view key) const {
    return {this, key};
  }

  // Gets the number of registered keys in the dictionary
  size_t num_keys() const {
    return num_keys_;
  }

  // Gets whether a binary mode or not.
  bool bin_mode() const {
    return bin_mode_;
  }

  // Gets the size of alphabet drawing keys in the dictionary.
  size_t alphabet_size() const {
    return alphabet_.size();
  }

  // Gets the number of nodes including free nodes.
  size_t num_nodes() const {
    return bc_.num_nodes();
  }

  // Gets the number of nodes in the original trie.
  size_t num_used_nodes() const {
    return bc_.num_used_nodes();
  }

  // Gets the number of free nodes corresponding to empty elements.
  size_t num_free_nodes() const {
    return bc_.num_free_nodes();
  }

  // Computes the output dictionary size in bytes.
  size_t size_in_bytes() const {
    size_t ret = 0;
    ret += bc_.size_in_bytes();
    ret += terminal_flags_.size_in_bytes();
    ret += tail_.size_in_bytes();
    ret += boundary_flags_.size_in_bytes();
    ret += alphabet_.size_in_bytes();
    ret += sizeof(table_);
    ret += sizeof(num_keys_);
    ret += sizeof(max_length_);
    ret += sizeof(bin_mode_);
    return ret;
  }

  // Reports the dictionary statistics into an ostream.
  void show_stat(std::ostream& os) const {
    const auto total_size = size_in_bytes();
    os << "basic statistics of xcdat::Trie<"
       << (Fast ? "true" : "false") << ">" << std::endl;
    show_size("\tnum keys:      ", num_keys(), os);
    show_size("\talphabet size: ", alphabet_size(), os);
    show_size("\tnum nodes:     ", num_nodes(), os);
    show_size("\tnum used nodes:", num_used_nodes(), os);
    show_size("\tnum free nodes:", num_free_nodes(), os);
    show_size("\tsize in bytes: ", size_in_bytes(), os);
    os << "member size statistics of xcdat::Trie<"
       << (Fast ? "true" : "false") << ">" << std::endl;
    show_size_ratio("\tbc:            ", bc_.size_in_bytes(), total_size, os);
    show_size_ratio("\tterminal_flags:", terminal_flags_.size_in_bytes(),
                    total_size, os);
    show_size_ratio("\ttail:          ", tail_.size_in_bytes(), total_size, os);
    show_size_ratio("\tboundary_flags:", boundary_flags_.size_in_bytes(),
                    total_size, os);
    bc_.show_stat(os);
  }

  // Writes the dictionary into an ostream.
  void write(std::ostream& os) const {
    bc_.write(os);
    terminal_flags_.write(os);
    tail_.write(os);
    boundary_flags_.write(os);
    alphabet_.write(os);
    os.write(reinterpret_cast<const char*>(table_), 512);
    write_value(num_keys_, os);
    write_value(max_length_, os);
    write_value(bin_mode_, os);
  }

  // Swap
  void swap(Trie& rhs) {
    std::swap(*this, rhs);
  }

  Trie(const Trie&) = delete;
  Trie& operator=(const Trie&) = delete;

  Trie(Trie&&) noexcept = default;
  Trie& operator=(Trie&&) noexcept = default;

private:
  bc_type bc_{};
  BitVector terminal_flags_{};
  Vector<char> tail_{};
  BitVector boundary_flags_{}; // used if binary_mode_ == true
  Vector<uint8_t> alphabet_{};
  uint8_t table_[512]{}; // table[table[c] + 256] = c

  size_t num_keys_{};
  size_t max_length_{};
  bool bin_mode_{};

  id_type to_key_id_(id_type node_id) const {
    return terminal_flags_.rank(node_id);
  };
  id_type to_node_id_(id_type string_id) const {
    return terminal_flags_.select(string_id);
  };
  id_type code_(char c) const {
    return table_[static_cast<uint8_t>(c)];
  }
  char edge_(id_type node_id, id_type child_id) const {
    return static_cast<char>(table_[(bc_.base(node_id) ^ child_id) + 256]);
  }

  bool match_suffix_(std::string_view key, size_t pos, size_t tail_pos) const {
    assert(pos <= key.length());

    if (pos == key.length()) {
      return tail_pos == 0;
    }

    if (bin_mode_) {
      do {
        if (key[pos] != tail_[tail_pos]) {
          return false;
        }
        ++pos;
        if (boundary_flags_[tail_pos]) {
          return pos == key.length();
        }
        ++tail_pos;
      } while (pos < key.length());
      return false;
    } else {
      do {
        if (!tail_[tail_pos] || key[pos] != tail_[tail_pos]) {
          return false;
        }
        ++pos;
        ++tail_pos;
      } while (pos < key.length());
      return !tail_[tail_pos];
    }
  }

  void extract_suffix_(size_t tail_pos, std::string& dec) const {
    if (bin_mode_) {
      if (tail_pos != 0) {
        do {
          dec += tail_[tail_pos];
        } while (!boundary_flags_[tail_pos++]);
      }
    } else {
      while (tail_[tail_pos] != '\0') {
        dec += tail_[tail_pos];
        ++tail_pos;
      }
    }
  }

  bool next_prefix_(PrefixIterator* it) const {
    if (it->end_flag_) {
      return false;
    }

    if (it->begin_flag_) {
      it->begin_flag_ = false;
      if (terminal_flags_[it->node_id_]) {
        it->id_ = to_key_id_(it->node_id_);
        return true;
      }
    }

    while (!bc_.is_leaf(it->node_id_)) {
      id_type child_id = bc_.base(it->node_id_) ^code_(it->key_[it->pos_++]);
      if (bc_.check(child_id) != it->node_id_) {
        it->end_flag_ = true;
        it->id_ = NOT_FOUND;
        return false;
      }
      it->node_id_ = child_id;
      if (!bc_.is_leaf(it->node_id_) && terminal_flags_[it->node_id_]) {
        it->id_ = to_key_id_(it->node_id_);
        return true;
      }
    }

    it->end_flag_ = true;
    size_t tail_pos = bc_.link(it->node_id_);

    if (!match_suffix_(it->key_, it->pos_, tail_pos)) {
      it->id_ = NOT_FOUND;
      return false;
    }

    it->pos_ = it->key_.length();
    it->id_ = to_key_id_(it->node_id_);
    return true;
  }

  bool next_predictive_(PredictiveIterator* it) const {
    if (it->end_flag_) {
      return false;
    }

    if (it->begin_flag_) {
      it->begin_flag_ = false;

      id_type node_id = 0;
      size_t pos = 0;

      for (; pos < it->key_.length(); ++pos) {
        if (bc_.is_leaf(node_id)) {
          it->end_flag_ = true;

          size_t tail_pos = bc_.link(node_id);
          if (tail_pos == 0) {
            return false;
          }

          if (bin_mode_) {
            do {
              if (it->key_[pos] != tail_[tail_pos]) {
                return false;
              }
              it->buf_ += it->key_[pos++];
              if (boundary_flags_[tail_pos]) {
                if (pos == it->key_.length()) {
                  it->id_ = to_key_id_(node_id);
                  return true;
                }
                return false;
              }
              ++tail_pos;
            } while (pos < it->key_.length());
          } else {
            do {
              if (it->key_[pos] != tail_[tail_pos] || !tail_[tail_pos]) {
                return false;
              }
              it->buf_ += it->key_[pos++];
              ++tail_pos;
            } while (pos < it->key_.length());
          }

          it->id_ = to_key_id_(node_id);
          extract_suffix_(tail_pos, it->buf_);
          return true;
        }

        id_type child_id = bc_.base(node_id) ^code_(it->key_[pos]);

        if (bc_.check(child_id) != node_id) {
          it->end_flag_ = true;
          return false;
        }

        node_id = child_id;
        it->buf_ += it->key_[pos];
      }

      if (!it->buf_.empty()) {
        it->stack_.push_back({pos, it->buf_.back(), node_id});
      } else {
        it->stack_.push_back({pos, '\0', node_id});
      }
    }

    while (!it->stack_.empty()) {
      id_type node_id = it->stack_.back().node_id;
      size_t depth = it->stack_.back().depth;
      uint8_t c = it->stack_.back().c;
      it->stack_.pop_back();

      if (0 < depth) {
        it->buf_.resize(depth);
        it->buf_.back() = c;
      }

      if (bc_.is_leaf(node_id)) {
        it->id_ = to_key_id_(node_id);
        extract_suffix_(bc_.link(node_id), it->buf_);
        return true;
      }

      const id_type base = bc_.base(node_id);

      // For lex sort
      for (auto rit = std::rbegin(alphabet_);
           rit != std::rend(alphabet_); ++rit) {
        const id_type child_id = base ^code_(*rit);
        if (bc_.check(child_id) == node_id) {
          it->stack_.push_back(
            {depth + 1, static_cast<char>(*rit), child_id}
          );
        }
      }

      if (terminal_flags_[node_id]) {
        it->id_ = to_key_id_(node_id);
        return true;
      }
    }

    it->end_flag_ = true;
    return false;
  }

  friend class TrieBuilder;
};

} //namespace - xcdat

#endif //XCDAT_TRIE_HPP_
