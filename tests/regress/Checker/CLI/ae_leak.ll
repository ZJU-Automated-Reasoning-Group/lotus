declare i8* @malloc(i64)

define i32 @main() {
entry:
  %allocation = call i8* @malloc(i64 8)
  ret i32 0
}
