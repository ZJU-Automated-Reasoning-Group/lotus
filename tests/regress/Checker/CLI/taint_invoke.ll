declare i32 @source()
declare void @sink(i32)
declare i32 @__gxx_personality_v0(...)

define i32 @main() personality i32 (...)* @__gxx_personality_v0 {
entry:
  %value = invoke i32 @source()
      to label %source.cont unwind label %unwind

source.cont:
  invoke void @sink(i32 %value)
      to label %done unwind label %unwind

done:
  ret i32 0

unwind:
  %exception = landingpad { i8*, i32 }
      cleanup
  resume { i8*, i32 } %exception
}
