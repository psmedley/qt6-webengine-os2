// v-0030 - This fails because 'RTArr' is a runtime array alias and it is used as store type.

type RTArr = [[stride(4)]] array<f32>;
[[set(0), binding(1)]] var<storage> x : RTArr;

[[stage(vertex)]]
fn main() -> void {
  return;
}
