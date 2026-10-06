declare void @__quantum__qis__x__body(ptr)
declare void @__quantum__qis__reset__body(ptr)
declare void @__quantum__qis__cx__body(ptr, ptr)
declare void @__quantum__qis__mz__body(ptr, ptr)
declare i1 @__quantum__rt__read_result(ptr)
declare void @__quantum__rt__result_record_output(ptr, ptr)
declare void @reset_decoder_ui64(i64)
declare void @enqueue_syndromes_ui64(i64, i64, i64, i64)
declare i64 @get_corrections_ui64(i64, i64, i64)

define void @repetition() #0 {
entry:
  call void @__quantum__qis__x__body(ptr inttoptr (i64 1 to ptr))
  br label %round
round:
  %r = phi i32 [0, %entry], [%next, %finish]
  call void @reset_decoder_ui64(i64 0)
  call void @__quantum__qis__reset__body(ptr inttoptr (i64 3 to ptr))
  call void @__quantum__qis__reset__body(ptr inttoptr (i64 4 to ptr))
  call void @__quantum__qis__cx__body(ptr null, ptr inttoptr (i64 3 to ptr))
  call void @__quantum__qis__cx__body(ptr inttoptr (i64 1 to ptr), ptr inttoptr (i64 3 to ptr))
  call void @__quantum__qis__cx__body(ptr inttoptr (i64 1 to ptr), ptr inttoptr (i64 4 to ptr))
  call void @__quantum__qis__cx__body(ptr inttoptr (i64 2 to ptr), ptr inttoptr (i64 4 to ptr))
  call void @__quantum__qis__mz__body(ptr inttoptr (i64 3 to ptr), ptr null)
  %s0 = call i1 @__quantum__rt__read_result(ptr null)
  call void @__quantum__rt__result_record_output(ptr null, ptr null)
  %w0 = zext i1 %s0 to i64
  call void @__quantum__qis__mz__body(ptr inttoptr (i64 4 to ptr), ptr inttoptr (i64 1 to ptr))
  %s1 = call i1 @__quantum__rt__read_result(ptr inttoptr (i64 1 to ptr))
  call void @__quantum__rt__result_record_output(ptr inttoptr (i64 1 to ptr), ptr null)
  %w1 = zext i1 %s1 to i64
  %high = shl i64 %w1, 1
  %bits = or i64 %w0, %high
  %tag = zext i32 %r to i64
  call void @enqueue_syndromes_ui64(i64 0, i64 2, i64 %bits, i64 %tag)
  %mask = call i64 @get_corrections_ui64(i64 0, i64 3, i64 1)
  br label %test0
test0:
  %shift0 = lshr i64 %mask, 0
  %flip0 = trunc i64 %shift0 to i1
  br i1 %flip0, label %correct0, label %test1
correct0:
  call void @__quantum__qis__x__body(ptr null)
  br label %test1
test1:
  %shift1 = lshr i64 %mask, 1
  %flip1 = trunc i64 %shift1 to i1
  br i1 %flip1, label %correct1, label %test2
correct1:
  call void @__quantum__qis__x__body(ptr inttoptr (i64 1 to ptr))
  br label %test2
test2:
  %shift2 = lshr i64 %mask, 2
  %flip2 = trunc i64 %shift2 to i1
  br i1 %flip2, label %correct2, label %finish
correct2:
  call void @__quantum__qis__x__body(ptr inttoptr (i64 2 to ptr))
  br label %finish
finish:
  call void @__quantum__qis__mz__body(ptr null, ptr inttoptr (i64 2 to ptr))
  call void @__quantum__rt__result_record_output(ptr inttoptr (i64 2 to ptr), ptr null)
  call void @__quantum__qis__mz__body(ptr inttoptr (i64 1 to ptr), ptr inttoptr (i64 3 to ptr))
  call void @__quantum__rt__result_record_output(ptr inttoptr (i64 3 to ptr), ptr null)
  call void @__quantum__qis__mz__body(ptr inttoptr (i64 2 to ptr), ptr inttoptr (i64 4 to ptr))
  call void @__quantum__rt__result_record_output(ptr inttoptr (i64 4 to ptr), ptr null)
  %next = add i32 %r, 1
  %more = icmp ult i32 %next, 3
  br i1 %more, label %round, label %end
end:
  ret void
}

attributes #0 = { "entry_point" "qir_profiles"="adaptive_profile" "required_num_qubits"="5" "required_num_results"="5" }
!llvm.module.flags = !{!0, !1, !2, !3}
!0 = !{i32 1, !"qir_major_version", i32 1}
!1 = !{i32 7, !"qir_minor_version", i32 0}
!2 = !{i32 1, !"dynamic_qubit_management", i1 false}
!3 = !{i32 1, !"dynamic_result_management", i1 false}
