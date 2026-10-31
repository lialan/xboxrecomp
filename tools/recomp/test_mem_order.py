"""--ordered-mem: which guest accesses stay ordered (MEMn/STn) and which are
the thread's own stack (MEMn_STK/STn_STK) -- mem_order.py over C text."""
import unittest

from .mem_order import mem_order_function, rewrite


class MemOrderTest(unittest.TestCase):
    def test_store_becomes_a_statement(self):
        self.assertEqual(rewrite("MEM32(esi + 8) = eax;", False), "ST32(esi + 8, eax);")

    def test_esp_access_is_the_stacks(self):
        self.assertEqual(rewrite("MEM32(esp + 4) = eax;", False), "ST32_STK(esp + 4, eax);")
        self.assertEqual(rewrite("eax = MEM32(esp);", False), "eax = MEM32_STK(esp);")

    def test_ebp_only_with_a_frame(self):
        self.assertEqual(rewrite("ecx = SMEM16(ebp - 8);", True), "ecx = SMEM16_STK(ebp - 8);")
        self.assertEqual(rewrite("ecx = SMEM16(ebp - 8);", False), "ecx = SMEM16(ebp - 8);")
        self.assertEqual(rewrite("MEM8(ebp + 3) = 1;", False), "ST8(ebp + 3, 1);")

    def test_nested_accesses_in_address_and_value(self):
        self.assertEqual(
            rewrite("MEM8(MEM32(esp + 4) + ecx) = (uint8_t)MEM16(edx);", False),
            "ST8(MEM32_STK(esp + 4) + ecx, (uint8_t)MEM16(edx));")

    def test_comparison_is_not_a_store(self):
        self.assertEqual(rewrite("_flags = (MEM32(esi) == MEM32(edi));", False),
                         "_flags = (MEM32(esi) == MEM32(edi));")

    def test_store_in_a_loop_body_and_a_conditional_value(self):
        self.assertEqual(
            rewrite("for (_i = 0; _i < ecx; _i++) MEM32(edi + _i*_st) = eax;", False),
            "for (_i = 0; _i < ecx; _i++) ST32(edi + _i*_st, eax);")
        self.assertEqual(rewrite("MEM16(esi) = c ? 1 : 2; esi += 2;", False),
                         "ST16(esi, c ? 1 : 2); esi += 2;")

    def test_index_off_esp_is_still_the_stack(self):
        self.assertEqual(rewrite("eax = MEM32(esp + ecx * 4 + 0x10);", False),
                         "eax = MEM32_STK(esp + ecx * 4 + 0x10);")

    def test_only_the_body_is_rewritten(self):
        code = "void sub_00001000(void)\n{\n    MEM32(esp) = 1;\n}\n"
        self.assertEqual(mem_order_function(code, False),
                         "void sub_00001000(void)\n{\n    ST32_STK(esp, 1);\n}\n")

    def test_identifiers_ending_in_mem_are_left_alone(self):
        self.assertEqual(rewrite("x = XMM_MEM(esi);", False), "x = XMM_MEM(esi);")


if __name__ == "__main__":
    unittest.main()
