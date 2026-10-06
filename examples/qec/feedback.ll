declare void @__quantum__qis__x__body(ptr)
declare void @__quantum__qis__mz__body(ptr, ptr)
declare i1 @__quantum__rt__read_result(ptr)
declare void @__quantum__rt__result_record_output(ptr, ptr)

define void @feedback() #0 {
entry:
  call void @__quantum__qis__x__body(ptr null)
  br label %round
round:
  %n = phi i32 [0, %entry], [%next, %join]
  call void @__quantum__qis__mz__body(ptr null, ptr null)
  %bit = call i1 @__quantum__rt__read_result(ptr null)
  call void @__quantum__rt__result_record_output(ptr null, ptr null)
  br i1 %bit, label %correct, label %join
correct:
  call void @__quantum__qis__x__body(ptr null)
  br label %join
join:
  %next = add i32 %n, 1
  %more = icmp ult i32 %next, 3
  br i1 %more, label %round, label %end
end:
  ret void
}

attributes #0 = { "entry_point" "qir_profiles"="adaptive_profile" "required_num_qubits"="1" "required_num_results"="1" }
!llvm.module.flags = !{!0, !1, !2, !3}
!0 = !{i32 1, !"qir_major_version", i32 1}
!1 = !{i32 7, !"qir_minor_version", i32 0}
!2 = !{i32 1, !"dynamic_qubit_management", i1 false}
!3 = !{i32 1, !"dynamic_result_management", i1 false}
