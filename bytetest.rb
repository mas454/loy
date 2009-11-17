# encoding: utf-8
# good_example.rb

# ヘッダ
header = [
  "YARVInstructionSequence/SimpleDataFormat", 1, 1, 1,
  { :arg_size=>0, :local_size=>1, :stack_max=>3 },
  "<dummy>", "foo.rb", :top, [], 0, []
]

# バイトコード本体 (スタックマシン)
body = [
  [:putnil],                   # レシーバを積む
                               # (関数呼び出しのときは nil)
  [:putobject, 1],             # 1 を積む
  [:send, :p, 1, nil, 8, nil], # p を呼ぶ (引数 1 個)
  [:leave]                     # 終わり
]

bytecode = header + [body]

# バイトコードを読み込む
iseq = RubyVM::InstructionSequence.load(bytecode)

# 実行する
iseq.eval  #=> p 1

