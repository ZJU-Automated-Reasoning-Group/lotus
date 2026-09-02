declare i32 @source()
declare void @sink(i32)

define i32 @main() {
entry:
  %value = call i32 @source()
  call void @sink(i32 %value)
  ret i32 0
}
