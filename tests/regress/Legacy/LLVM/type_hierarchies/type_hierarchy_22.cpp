struct Top {
  virtual ~Top() = default;
};

struct Left : Top {};
struct Right : Top {};
struct Bottom : Left, Right {};

int main() {
  Bottom Value;
  return sizeof(Value);
}
