; ModuleID = 'LLVMDialectModule'
source_filename = "LLVMDialectModule"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"

@cstr.72303030303000 = private constant [7 x i8] c"r00000\00"
@cstr.72303030303100 = private constant [7 x i8] c"r00001\00"
@cstr.61727261793C6931207820323E00 = private constant [14 x i8] c"array<i1 x 2>\00"

define void @__nvqpp__mlirgen__bell..0x7e060f6760c0() #0 {
  call void @__quantum__qis__h__body(ptr null)
  call void @__quantum__qis__cnot__body(ptr null, ptr inttoptr (i64 1 to ptr))
  call void @__quantum__qis__mz__body(ptr null, ptr null)
  call void @__quantum__qis__mz__body(ptr inttoptr (i64 1 to ptr), ptr inttoptr (i64 1 to ptr))
  call void @__quantum__rt__array_record_output(i64 2, ptr @cstr.61727261793C6931207820323E00)
  call void @__quantum__rt__result_record_output(ptr null, ptr @cstr.72303030303000)
  call void @__quantum__rt__result_record_output(ptr inttoptr (i64 1 to ptr), ptr @cstr.72303030303100)
  ret void
}

declare void @__quantum__qis__h__body(ptr)

declare void @__quantum__qis__mz__body(ptr, ptr) #1

declare void @__quantum__rt__result_record_output(ptr, ptr)

declare void @__quantum__qis__cnot__body(ptr, ptr)

declare void @__quantum__rt__array_record_output(i64, ptr)

attributes #0 = { "entry_point" "output_labeling_schema"="schema_id" "output_names"="[[[0,[0,\22r00000\22]],[1,[1,\22r00001\22]]]]" "qir_profiles"="base_profile" "requiredQubits"="2" "requiredResults"="2" }
attributes #1 = { "irreversible" }

!llvm.module.flags = !{!0}

!0 = !{i32 2, !"Debug Info Version", i32 3}
