# Detecting missing / wrong placements with a local AI model

A working note, so it can be picked up separately from the machine work. Nothing here is built or tested yet.

## 1. Does it make sense?

**Yes, but not as the first step.** What we have are pictures of every placement location (close-ups, cropped to the
part footprint + 2 mm, at `before`, `during` and `after`). The question "is a part sitting here?" is a small
**image classification** problem. Two ways to solve it:

| | Classic image comparison | Trained model (small CNN) |
|---|---|---|
| Idea | Compare the `before` close-up (paste / bare pads) with the `during` close-up of the same spot. A part appearing changes the picture a lot. | Learn what "part present", "missing" (and later "tombstoned", "wrong part", "shifted") look like. |
| Training data | None | Hundreds of labelled pictures |
| Effort | An hour or two | Days (mostly data collection) |
| Works for new parts | Yes (it only looks for change) | Only if it has seen similar ones |
| Weak points | Lighting changes, reflections, a shifted camera | Needs examples of every failure you care about |

**Recommendation:** start with the classic comparison as a baseline. It may already be good enough for "missing / not
missing". Only train a model for what the baseline gets wrong, or for defects after reflow (tombstones, bridges,
rotated parts), where there is nothing to compare with.

A model trained on `before` + `during` **pairs** (see 5.3) is the best of both: it learns "did something appear?",
so it needs no per-part training.

Note that OpenPnP already knows a part was placed (vacuum check, bottom vision). This check mostly adds value
(a) for parts lost on the way or flipped, and (b) after reflow, which OpenPnP cannot see.

## 2. Hardware

- **Ryzen AI 9** laptop: more than enough. Models of this size run in about 10 ms per picture on the CPU.
- Training on the **CPU** is fine for a few thousand small pictures (minutes per epoch). The integrated GPU or the
  NPU is optional and not worth the setup time for a first version.
- The NPU (Ryzen AI Software, ONNX Runtime with the Vitis AI provider) can run the finished model, but you would
  not notice the difference. Skip it at first.

## 3. Software

Windows, PowerShell, Python 3.11 or 3.12:

```powershell
py -3.12 -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install torch torchvision --index-url https://download.pytorch.org/whl/cpu
pip install onnxruntime opencv-python pillow scikit-learn matplotlib
```

- **PyTorch + torchvision**: training (use a pretrained **MobileNetV3-Small** or **ResNet18**, fine-tuned).
- **ONNX Runtime**: running the exported model later (small, fast, no PyTorch needed at run time).
- Optional: **FiftyOne** or **Label Studio** to review and fix labels.

## 4. Data: what we already have, what is missing

The snapshot scripts write (per batch and board):

```
Batch-N\<board id>\placements\R1_before.jpg  R1_during.jpg  R1_after.jpg  index_<stage>.csv
```

**To collect for training:**
1. Switch the **before** stage to `MODE = 'both'` so it also saves close-ups (default is panorama only).
2. Run jobs where some parts are **left out on purpose** (disable placements, or pull parts from a few spots).
   Write down which designators were skipped, so the labels are certain.
3. Vary things on purpose: different lighting, boards, part values, paste amounts, rotations.

**Labels** (simplest possible): folders.

```
dataset\
  present\   R1_during_b3.jpg ...
  missing\   C5_during_b3.jpg ...
```

A small script can sort the pictures into these folders from a CSV of "skipped designators per batch".

**How much:** about 200–500 pictures per class to start (more helps), from **several different boards / batches**.

Split train / validation / test **by batch or board, not by picture**. Pictures of the same spot are nearly
identical; a random split would look great on paper and fail on the next board.

## 5. Plan

### 5.1 Baseline without training (do this first)

For each placement, take `before` and `during`, convert to grey, blur a little, and compute the mean absolute
difference (or SSIM) inside the footprint area. A large change means a part is there. Pick the threshold per
package size on the collected data, and print a table: designator, score, verdict.

Judge it with a **confusion matrix** on the labelled data. If it finds nearly all missing parts, you can stop here.

### 5.2 Single-picture classifier

Fine-tune MobileNetV3-Small on `during` crops: classes `present` / `missing`. Resize to 224 × 224, with padding
rather than stretching (crops have different aspect ratios). Augment lightly: brightness, small shifts, small blur.
Do **not** flip or rotate strongly if orientation matters.

### 5.3 Pair classifier (recommended end point)

Feed `before` and `during` of the same spot together: either 6 input channels, or the picture plus the
difference picture. It learns "something changed here", so it works for part values it has never seen.

### 5.4 Reflow defects (later)

Same idea on `after` crops with classes such as `ok`, `tombstone`, `missing`, `bridge`. This needs defect
examples. Plan to collect them over weeks, or fake them.

## 6. Training skeleton (single-picture classifier)

```python
import torch, torchvision
from torchvision import transforms as T, datasets

tf_train = T.Compose([T.Resize((224, 224)), T.ColorJitter(0.3, 0.3, 0.2), T.ToTensor()])
tf_eval  = T.Compose([T.Resize((224, 224)), T.ToTensor()])
train = datasets.ImageFolder("dataset/train", tf_train)
val   = datasets.ImageFolder("dataset/val",   tf_eval)

model = torchvision.models.mobilenet_v3_small(weights="DEFAULT")
model.classifier[-1] = torch.nn.Linear(model.classifier[-1].in_features, len(train.classes))

opt = torch.optim.AdamW(model.parameters(), lr=1e-3)
loss_fn = torch.nn.CrossEntropyLoss()
dl = torch.utils.data.DataLoader(train, batch_size=32, shuffle=True)
for epoch in range(15):
    model.train()
    for x, y in dl:
        opt.zero_grad(); loss_fn(model(x), y).backward(); opt.step()
    # evaluate on `val` here, print the confusion matrix, keep the best epoch

torch.onnx.export(model.eval(), torch.randn(1, 3, 224, 224), "placement.onnx",
                  input_names=["x"], output_names=["logits"])
```

## 7. Using it

1. **Offline first.** A Python script takes a batch folder and writes `report.csv`
   (designator, score, verdict) and a picture with the missing spots circled. No machine integration needed.
2. Later, call it from the OpenPnP end-of-job event script (a command line call to the Python script) and show the
   verdict in the log.
3. Decide the threshold from the **cost of errors**: a missed missing part is worse than a false alarm, so bias the
   threshold toward flagging.

## 8. Pitfalls

- **Lighting changes** (head camera light, exposure, white balance). Keep the capture settings fixed and include
  both bright and dark examples in the training data.
- **Different crop sizes** per package: always resize with padding.
- **Camera not exactly on the spot** (a few hundredths of a mm): usually fine at these crop sizes, but check the
  `before` / `during` pair alignment by eye.
- **Small data sets lie.** Hold out whole batches and look at the actual mistakes, not only the accuracy number.
- **0402 parts** are tiny in the crop. If it struggles, take close-ups at higher zoom for those packages.

## 9. Suggested order

1. Switch `before` to `'both'` and run a couple of batches with parts deliberately left out.
2. Build the baseline comparison and look at the confusion matrix.
3. Only if that is not good enough: collect more data and train the single-picture model, then the pair model.
4. Wire the result into the job end event.

## 10. Time check

The Maker Faire is close. This is a good *after the Faire* project. If you want something to **show** at the Faire,
the baseline in 5.1 can be done quickly and shown on the overview pictures, with no training at all.
