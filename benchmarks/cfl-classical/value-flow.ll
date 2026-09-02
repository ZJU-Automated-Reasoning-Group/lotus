define i8* @identity(i8* %value) {
entry:
  ret i8* %value
}

define i8* @main() {
entry:
  %seed = alloca i8
  %result = call i8* @identity(i8* %seed)
  ret i8* %result
}
