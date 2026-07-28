import torch
from torch import nn


class TwoLinearLayerApplication(nn.Module):
    def __init__(self):
        super().__init__()
        self.first = nn.Linear(4, 3)
        self.second = nn.Linear(3, 2)

        with torch.no_grad():
            self.first.weight.copy_(
                torch.tensor(
                    [
                        [1.0, 0.0, 0.0, 0.0],
                        [0.0, 1.0, 1.0, 0.0],
                        [0.0, 0.0, 0.0, 1.0],
                    ]
                )
            )
            self.first.bias.copy_(torch.tensor([1.0, -1.0, 2.0]))
            self.second.weight.copy_(
                torch.tensor(
                    [
                        [1.0, 1.0, 1.0],
                        [2.0, -1.0, 0.5],
                    ]
                )
            )
            self.second.bias.copy_(torch.tensor([0.0, 1.0]))

    def forward(self, value):
        return self.second(self.first(value))


SAMPLE_INPUT = torch.tensor([[1.0, 2.0, 3.0, 4.0]], dtype=torch.float32)
EXPECTED = torch.tensor([[12.0, 4.0]], dtype=torch.float32)


if __name__ == "__main__":
    result = TwoLinearLayerApplication()(SAMPLE_INPUT)
    print(f"PyTorch result: {result.tolist()}")
    if not torch.equal(result, EXPECTED):
        raise SystemExit("unexpected PyTorch result")
