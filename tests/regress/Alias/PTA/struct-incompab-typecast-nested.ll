; ModuleID = 'basic_c_tests/struct-incompab-typecast-nested.c'
source_filename = "basic_c_tests/struct-incompab-typecast-nested.c"
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

%struct.DstStruct = type { [10 x i32*], [10 x i8], [5 x %struct.InnerStruct] }
%struct.InnerStruct = type { i8, i32* }
%struct.SrcStruct = type { [10 x i32*], [10 x i8], [5 x %struct.InnerStruct], i8 }

; Function Attrs: noinline nounwind optnone uwtable
define dso_local i32 @main() #0 !dbg !9 {
  %1 = alloca i32, align 4
  %2 = alloca %struct.DstStruct*, align 8
  %3 = alloca %struct.SrcStruct*, align 8
  %4 = alloca %struct.SrcStruct, align 8
  %5 = alloca i32, align 4
  %6 = alloca i32, align 4
  %7 = alloca i32, align 4
  store i32 0, i32* %1, align 4
  call void @llvm.dbg.declare(metadata %struct.DstStruct** %2, metadata !13, metadata !DIExpression()), !dbg !33
  call void @llvm.dbg.declare(metadata %struct.SrcStruct** %3, metadata !34, metadata !DIExpression()), !dbg !42
  call void @llvm.dbg.declare(metadata %struct.SrcStruct* %4, metadata !43, metadata !DIExpression()), !dbg !44
  call void @llvm.dbg.declare(metadata i32* %5, metadata !45, metadata !DIExpression()), !dbg !46
  call void @llvm.dbg.declare(metadata i32* %6, metadata !47, metadata !DIExpression()), !dbg !48
  call void @llvm.dbg.declare(metadata i32* %7, metadata !49, metadata !DIExpression()), !dbg !50
  store %struct.SrcStruct* %4, %struct.SrcStruct** %3, align 8, !dbg !51
  %8 = load %struct.SrcStruct*, %struct.SrcStruct** %3, align 8, !dbg !52
  %9 = getelementptr inbounds %struct.SrcStruct, %struct.SrcStruct* %8, i32 0, i32 0, !dbg !53
  %10 = getelementptr inbounds [10 x i32*], [10 x i32*]* %9, i64 0, i64 3, !dbg !52
  store i32* %5, i32** %10, align 8, !dbg !54
  %11 = load %struct.SrcStruct*, %struct.SrcStruct** %3, align 8, !dbg !55
  %12 = getelementptr inbounds %struct.SrcStruct, %struct.SrcStruct* %11, i32 0, i32 2, !dbg !56
  %13 = getelementptr inbounds [5 x %struct.InnerStruct], [5 x %struct.InnerStruct]* %12, i64 0, i64 2, !dbg !55
  %14 = getelementptr inbounds %struct.InnerStruct, %struct.InnerStruct* %13, i32 0, i32 1, !dbg !57
  store i32* %6, i32** %14, align 8, !dbg !58
  %15 = load %struct.SrcStruct*, %struct.SrcStruct** %3, align 8, !dbg !59
  %16 = bitcast %struct.SrcStruct* %15 to %struct.DstStruct*, !dbg !59
  store %struct.DstStruct* %16, %struct.DstStruct** %2, align 8, !dbg !60
  %17 = load %struct.DstStruct*, %struct.DstStruct** %2, align 8, !dbg !61
  %18 = getelementptr inbounds %struct.DstStruct, %struct.DstStruct* %17, i32 0, i32 0, !dbg !61
  %19 = getelementptr inbounds [10 x i32*], [10 x i32*]* %18, i64 0, i64 9, !dbg !61
  %20 = load i32*, i32** %19, align 8, !dbg !61
  %21 = bitcast i32* %20 to i8*, !dbg !61
  %22 = bitcast i32* %5 to i8*, !dbg !61
  call void @__aser_alias__(i8* %21, i8* %22), !dbg !61
  %23 = load %struct.DstStruct*, %struct.DstStruct** %2, align 8, !dbg !62
  %24 = getelementptr inbounds %struct.DstStruct, %struct.DstStruct* %23, i32 0, i32 2, !dbg !62
  %25 = getelementptr inbounds [5 x %struct.InnerStruct], [5 x %struct.InnerStruct]* %24, i64 0, i64 3, !dbg !62
  %26 = getelementptr inbounds %struct.InnerStruct, %struct.InnerStruct* %25, i32 0, i32 1, !dbg !62
  %27 = load i32*, i32** %26, align 8, !dbg !62
  %28 = bitcast i32* %27 to i8*, !dbg !62
  %29 = bitcast i32* %6 to i8*, !dbg !62
  call void @__aser_alias__(i8* %28, i8* %29), !dbg !62
  %30 = load %struct.SrcStruct*, %struct.SrcStruct** %3, align 8, !dbg !63
  %31 = getelementptr inbounds %struct.SrcStruct, %struct.SrcStruct* %30, i32 0, i32 0, !dbg !63
  %32 = getelementptr inbounds [10 x i32*], [10 x i32*]* %31, i64 0, i64 2, !dbg !63
  %33 = load i32*, i32** %32, align 8, !dbg !63
  %34 = bitcast i32* %33 to i8*, !dbg !63
  %35 = bitcast i32* %7 to i8*, !dbg !63
  call void @__aser_no_alias__(i8* %34, i8* %35), !dbg !63
  %36 = load %struct.DstStruct*, %struct.DstStruct** %2, align 8, !dbg !64
  %37 = getelementptr inbounds %struct.DstStruct, %struct.DstStruct* %36, i32 0, i32 2, !dbg !65
  %38 = getelementptr inbounds [5 x %struct.InnerStruct], [5 x %struct.InnerStruct]* %37, i64 0, i64 1, !dbg !64
  %39 = getelementptr inbounds %struct.InnerStruct, %struct.InnerStruct* %38, i32 0, i32 1, !dbg !66
  store i32* %7, i32** %39, align 8, !dbg !67
  %40 = load %struct.SrcStruct*, %struct.SrcStruct** %3, align 8, !dbg !68
  %41 = getelementptr inbounds %struct.SrcStruct, %struct.SrcStruct* %40, i32 0, i32 2, !dbg !68
  %42 = getelementptr inbounds [5 x %struct.InnerStruct], [5 x %struct.InnerStruct]* %41, i64 0, i64 1, !dbg !68
  %43 = getelementptr inbounds %struct.InnerStruct, %struct.InnerStruct* %42, i32 0, i32 1, !dbg !68
  %44 = load i32*, i32** %43, align 8, !dbg !68
  %45 = bitcast i32* %44 to i8*, !dbg !68
  %46 = bitcast i32* %7 to i8*, !dbg !68
  call void @__aser_alias__(i8* %45, i8* %46), !dbg !68
  ret i32 0, !dbg !69
}

; Function Attrs: nounwind readnone speculatable
declare void @llvm.dbg.declare(metadata, metadata, metadata) #1

declare dso_local void @__aser_alias__(i8*, i8*) #2

declare dso_local void @__aser_no_alias__(i8*, i8*) #2

attributes #0 = { noinline nounwind optnone uwtable "correctly-rounded-divide-sqrt-fp-math"="false" "disable-tail-calls"="false" "less-precise-fpmad"="false" "min-legal-vector-width"="0" "no-frame-pointer-elim"="true" "no-frame-pointer-elim-non-leaf" "no-infs-fp-math"="false" "no-jump-tables"="false" "no-nans-fp-math"="false" "no-signed-zeros-fp-math"="false" "no-trapping-math"="false" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #1 = { nounwind readnone speculatable }
attributes #2 = { "correctly-rounded-divide-sqrt-fp-math"="false" "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="true" "no-frame-pointer-elim-non-leaf" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "no-signed-zeros-fp-math"="false" "no-trapping-math"="false" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "unsafe-fp-math"="false" "use-soft-float"="false" }

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!5, !6, !7}
!llvm.ident = !{!8}

!0 = distinct !DICompileUnit(language: DW_LANG_C99, file: !1, producer: "clang version 9.0.0 (tags/RELEASE_900/final)", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, enums: !2, retainedTypes: !3, nameTableKind: None)
!1 = !DIFile(filename: "basic_c_tests/struct-incompab-typecast-nested.c", directory: "/home/peiming/Documents/Projects/LLVMRace/TestCases/PTABen")
!2 = !{}
!3 = !{!4}
!4 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: null, size: 64)
!5 = !{i32 2, !"Dwarf Version", i32 4}
!6 = !{i32 2, !"Debug Info Version", i32 3}
!7 = !{i32 1, !"wchar_size", i32 4}
!8 = !{!"clang version 9.0.0 (tags/RELEASE_900/final)"}
!9 = distinct !DISubprogram(name: "main", scope: !1, file: !1, line: 26, type: !10, scopeLine: 26, spFlags: DISPFlagDefinition, unit: !0, retainedNodes: !2)
!10 = !DISubroutineType(types: !11)
!11 = !{!12}
!12 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!13 = !DILocalVariable(name: "pdst", scope: !9, file: !1, line: 27, type: !14)
!14 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !15, size: 64)
!15 = distinct !DICompositeType(tag: DW_TAG_structure_type, name: "DstStruct", file: !1, line: 20, size: 1408, elements: !16)
!16 = !{!17, !22, !25}
!17 = !DIDerivedType(tag: DW_TAG_member, name: "f1", scope: !15, file: !1, line: 21, baseType: !18, size: 640)
!18 = !DICompositeType(tag: DW_TAG_array_type, baseType: !19, size: 640, elements: !20)
!19 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !12, size: 64)
!20 = !{!21}
!21 = !DISubrange(count: 10)
!22 = !DIDerivedType(tag: DW_TAG_member, name: "f2", scope: !15, file: !1, line: 22, baseType: !23, size: 80, offset: 640)
!23 = !DICompositeType(tag: DW_TAG_array_type, baseType: !24, size: 80, elements: !20)
!24 = !DIBasicType(name: "char", size: 8, encoding: DW_ATE_signed_char)
!25 = !DIDerivedType(tag: DW_TAG_member, name: "f3", scope: !15, file: !1, line: 23, baseType: !26, size: 640, offset: 768)
!26 = !DICompositeType(tag: DW_TAG_array_type, baseType: !27, size: 640, elements: !31)
!27 = distinct !DICompositeType(tag: DW_TAG_structure_type, name: "InnerStruct", file: !1, line: 8, size: 128, elements: !28)
!28 = !{!29, !30}
!29 = !DIDerivedType(tag: DW_TAG_member, name: "in1", scope: !27, file: !1, line: 9, baseType: !24, size: 8)
!30 = !DIDerivedType(tag: DW_TAG_member, name: "in2", scope: !27, file: !1, line: 10, baseType: !19, size: 64, offset: 64)
!31 = !{!32}
!32 = !DISubrange(count: 5)
!33 = !DILocation(line: 27, column: 20, scope: !9)
!34 = !DILocalVariable(name: "psrc", scope: !9, file: !1, line: 28, type: !35)
!35 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !36, size: 64)
!36 = distinct !DICompositeType(tag: DW_TAG_structure_type, name: "SrcStruct", file: !1, line: 13, size: 1472, elements: !37)
!37 = !{!38, !39, !40, !41}
!38 = !DIDerivedType(tag: DW_TAG_member, name: "f1", scope: !36, file: !1, line: 14, baseType: !18, size: 640)
!39 = !DIDerivedType(tag: DW_TAG_member, name: "f2", scope: !36, file: !1, line: 15, baseType: !23, size: 80, offset: 640)
!40 = !DIDerivedType(tag: DW_TAG_member, name: "f3", scope: !36, file: !1, line: 16, baseType: !26, size: 640, offset: 768)
!41 = !DIDerivedType(tag: DW_TAG_member, name: "f4", scope: !36, file: !1, line: 17, baseType: !24, size: 8, offset: 1408)
!42 = !DILocation(line: 28, column: 20, scope: !9)
!43 = !DILocalVariable(name: "s", scope: !9, file: !1, line: 29, type: !36)
!44 = !DILocation(line: 29, column: 19, scope: !9)
!45 = !DILocalVariable(name: "x", scope: !9, file: !1, line: 30, type: !12)
!46 = !DILocation(line: 30, column: 6, scope: !9)
!47 = !DILocalVariable(name: "y", scope: !9, file: !1, line: 30, type: !12)
!48 = !DILocation(line: 30, column: 9, scope: !9)
!49 = !DILocalVariable(name: "z", scope: !9, file: !1, line: 30, type: !12)
!50 = !DILocation(line: 30, column: 12, scope: !9)
!51 = !DILocation(line: 32, column: 7, scope: !9)
!52 = !DILocation(line: 33, column: 2, scope: !9)
!53 = !DILocation(line: 33, column: 8, scope: !9)
!54 = !DILocation(line: 33, column: 14, scope: !9)
!55 = !DILocation(line: 34, column: 2, scope: !9)
!56 = !DILocation(line: 34, column: 8, scope: !9)
!57 = !DILocation(line: 34, column: 14, scope: !9)
!58 = !DILocation(line: 34, column: 18, scope: !9)
!59 = !DILocation(line: 36, column: 9, scope: !9)
!60 = !DILocation(line: 36, column: 7, scope: !9)
!61 = !DILocation(line: 38, column: 2, scope: !9)
!62 = !DILocation(line: 39, column: 2, scope: !9)
!63 = !DILocation(line: 40, column: 2, scope: !9)
!64 = !DILocation(line: 42, column: 2, scope: !9)
!65 = !DILocation(line: 42, column: 8, scope: !9)
!66 = !DILocation(line: 42, column: 14, scope: !9)
!67 = !DILocation(line: 42, column: 18, scope: !9)
!68 = !DILocation(line: 43, column: 2, scope: !9)
!69 = !DILocation(line: 45, column: 2, scope: !9)
