; RUN: llc -O0 < %s | FileCheck %s
; Radar 10464995
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"
target triple = "x86_64-apple-macosx10.7.2"

@s = common global [4294967296 x i8] zeroinitializer, align 16
; CHECK: .quad 4294967296 ## DW_AT_count

define void @bar() nounwind uwtable ssp {
entry:
  store i8 97, i8* getelementptr inbounds ([4294967296 x i8], [4294967296 x i8]* @s, i32 0, i64 0), align 1, !dbg !18
  ret void, !dbg !20
}

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!22}

!0 = !MDCompileUnit(language: DW_LANG_C99, producer: "clang version 3.1 (trunk 144833)", isOptimized: false, emissionKind: 0, file: !21, enums: !1, retainedTypes: !1, subprograms: !3, globals: !11, imports:  !1)
!1 = !{}
!3 = !{!5}
!5 = !MDSubprogram(name: "bar", line: 4, isLocal: false, isDefinition: true, virtualIndex: 6, flags: DIFlagPrototyped, isOptimized: false, file: !21, scope: !6, type: !7, function: void ()* @bar)
!6 = !MDFile(filename: "small.c", directory: "/private/tmp")
!7 = !MDSubroutineType(types: !8)
!8 = !{null}
!11 = !{!13}
!13 = !MDGlobalVariable(name: "s", line: 2, isLocal: false, isDefinition: true, scope: null, file: !6, type: !14, variable: [4294967296 x i8]* @s)
!14 = !MDCompositeType(tag: DW_TAG_array_type, size: 34359738368, align: 8, baseType: !15, elements: !16)
!15 = !MDBasicType(tag: DW_TAG_base_type, name: "char", size: 8, align: 8, encoding: DW_ATE_signed_char)
!16 = !{!17}
!17 = !MDSubrange(count: 4294967296)
!18 = !MDLocation(line: 5, column: 3, scope: !19)
!19 = distinct !MDLexicalBlock(line: 4, column: 1, file: !21, scope: !5)
!20 = !MDLocation(line: 6, column: 1, scope: !19)
!21 = !MDFile(filename: "small.c", directory: "/private/tmp")
!22 = !{i32 1, !"Debug Info Version", i32 3}
