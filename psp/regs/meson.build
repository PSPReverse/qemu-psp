psp_regs_json = files(
  'generic.json',
  'zen.json',
  'zen_plus.json',
  'zen2.json',
  'zen3.json',
  'zen_tesla.json',
)

psp_regs_inc = custom_target('psp-regs.c.inc',
  input: psp_regs_json,
  output: 'psp-regs.c.inc',
  command: [python, files('gen_regs.py'), '--output', '@OUTPUT@', '@INPUT@'],
  depend_files: files('gen_regs.py'),
)

specific_ss.add(when: 'CONFIG_AMD_PSP', if_true: [files('psp-regs.c'), psp_regs_inc])
